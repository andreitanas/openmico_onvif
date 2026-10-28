#include "daynight.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "capture.h"
#include "util.h"

// Wyze Cam v2 GPIOs: IR LEDs, and the IR cut filter's two coil directions
// (pulsed for a second to move it).
#define GPIO_LEDS 49
#define GPIO_FILTER_IN 26  // day: filter in front of the sensor
#define GPIO_FILTER_OUT 25 // night
#define ISP_INFO "/proc/jz/isp/isp_info"

#define SETTLE_MS 5000         // after a switch, let exposure settle
#define NIGHT_AFTER_S 3        // seconds above NIGHT_EXPOSURE
#define DAY_AFTER_S 10         // seconds below DAY_EXPOSURE (no LEDs)
#define PROBE_MS 3500          // LEDs off this long per probe
#define PROBE_FIRST_MS 10000   // first probe after switching to night
#define PROBE_MIN_MS 30000     // probe interval: doubles up to the max
#define PROBE_MAX_MS 600000
#define PROBE_MOVED 100000     // exposure change that brings a probe forward
#define CUSTOM_STEP 50000      // exposure per icrCustomValue step
#define SHORT_DAY_MS 120000    // a day shorter than this was a mistake

static const struct config *cfg;
static pthread_t thread;
static volatile int running;

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static struct { // guarded by lock
    int manual, leds, lux, custom;
} want = {0, 1, 0, 2};

static int night = -1, leds_on = -1; // thread only

static void gpio_out(int gpio, int value) {
    char path[64];
    snprintf(path, sizeof(path), "/sys/class/gpio/gpio%d/value", gpio);
    if (access(path, F_OK) < 0) {
        FILE *fp = fopen("/sys/class/gpio/export", "w");
        if (fp) {
            fprintf(fp, "%d", gpio);
            fclose(fp);
        }
        snprintf(path, sizeof(path), "/sys/class/gpio/gpio%d/direction", gpio);
        fp = fopen(path, "w");
        if (fp) {
            fputs("out", fp);
            fclose(fp);
        }
        snprintf(path, sizeof(path), "/sys/class/gpio/gpio%d/value", gpio);
    }
    FILE *fp = fopen(path, "w");
    if (fp) {
        fprintf(fp, "%d", value);
        fclose(fp);
    }
}

static void set_leds(int on) {
    if (on != leds_on)
        gpio_out(GPIO_LEDS, on);
    leds_on = on;
}

static void set_night(int on, int leds) {
    if (on != night) {
        int coil = on ? GPIO_FILTER_OUT : GPIO_FILTER_IN;
        gpio_out(on ? GPIO_FILTER_IN : GPIO_FILTER_OUT, 0);
        gpio_out(coil, 1);
        sleep_ms(1000);
        gpio_out(coil, 0);
        capture_night(on);
        night = on;
    }
    set_leds(on && leds);
}

// The ISP's exposure figure ("exposure log2 id", larger is darker), -1 if
// unreadable: from the capture pipeline, else the vendor ISP's proc file.
static int exposure(void) {
    int e = capture_exposure();
    if (e >= 0)
        return e;
    FILE *fp = fopen(ISP_INFO, "r");
    if (!fp)
        return -1;
    char line[128];
    while (fgets(line, sizeof(line), fp)) {
        if (!strncmp(line, "ISP exposure log2 id", 20)) {
            char *c = strchr(line, ':');
            if (c)
                e = atoi(c + 1);
            break;
        }
    }
    fclose(fp);
    return e;
}

// The darkest exposure seen with the LEDs off for PROBE_MS. The LEDs stay
// off; the caller decides.
static int probe(void) {
    set_leds(0);
    int max = -1;
    for (int t = 0; t < PROBE_MS && running; t += 250) {
        sleep_ms(250);
        int e = exposure();
        if (e > max)
            max = e;
    }
    return max;
}

static void *daynight_main(void *arg) {
    int64_t now = mono_ms(), switched = now, day_since = 0;
    int64_t next_probe = 0, interval = PROBE_MIN_MS, last_probe = 0;
    int above = 0, below = 0, avg = 0, ref = 0;
    set_night(0, 0);
    while (running) {
        sleep_ms(1000);
        pthread_mutex_lock(&lock);
        int manual = want.manual, leds = want.leds, lux = want.lux, custom = want.custom;
        pthread_mutex_unlock(&lock);
        now = mono_ms();
        int was = night;

        if (manual) {
            set_night(leds, leds);
        } else {
            int shift = lux ? (custom - 2) * CUSTOM_STEP : 0;
            int night_exp = cfg->night_exposure - shift, day_exp = cfg->day_exposure - shift;
            int e = now - switched < SETTLE_MS ? -1 : exposure();
            if (e < 0) {
                // settling or unreadable
            } else if (!night) {
                above = e > night_exp ? above + 1 : 0;
                if (above >= NIGHT_AFTER_S) {
                    log_info("daynight: night, exposure %d > %d", e, night_exp);
                    set_night(1, leds);
                }
            } else if (!leds) {
                // No LEDs: the reading is the scene's own light.
                set_leds(0);
                below = e < day_exp ? below + 1 : 0;
                if (below >= DAY_AFTER_S) {
                    log_info("daynight: day, exposure %d < %d", e, day_exp);
                    set_night(0, 0);
                }
            } else if (!leds_on) {
                // LEDs just enabled from Protect
                set_leds(1);
                switched = now;
            } else {
                avg = avg ? (avg * 4 + e) / 5 : e;
                int moved = ref > 0 && abs(avg - ref) > PROBE_MOVED && now - last_probe >= PROBE_MIN_MS;
                // Even lit by the LEDs it reads dark: no light to find.
                if (e < day_exp && (now >= next_probe || moved)) {
                    int m = probe();
                    last_probe = mono_ms();
                    if (m >= 0 && m < day_exp) {
                        log_info("daynight: day, exposure %d < %d without IR LEDs", m, day_exp);
                        set_night(0, 0);
                    } else {
                        set_leds(1);
                        interval = interval * 2 < PROBE_MAX_MS ? interval * 2 : PROBE_MAX_MS;
                        next_probe = last_probe + interval;
                        log_info("daynight: still dark without IR LEDs (%d), next check in %d s",
                                 m, (int)(interval / 1000));
                        switched = last_probe; // let the LEDs' exposure settle again
                        avg = 0;
                        ref = -1; // set from the next settled reading
                    }
                } else if (ref < 0) {
                    ref = avg;
                }
            }
        }

        if (night != was) {
            switched = mono_ms();
            above = below = avg = ref = 0;
            if (night) {
                // Back to night soon after a probe said day: keep backing off.
                if (!day_since || switched - day_since >= SHORT_DAY_MS) {
                    interval = PROBE_MIN_MS / 2;
                    next_probe = switched + PROBE_FIRST_MS;
                } else {
                    interval = interval * 2 < PROBE_MAX_MS ? interval * 2 : PROBE_MAX_MS;
                    next_probe = switched + interval;
                }
            } else {
                day_since = switched;
            }
        }
    }
    return NULL;
}

void daynight_settings(int manual, int leds, int lux, int custom) {
    pthread_mutex_lock(&lock);
    int changed = manual != want.manual || leds != want.leds || lux != want.lux || custom != want.custom;
    want.manual = manual;
    want.leds = leds;
    want.lux = lux;
    want.custom = custom < 0 ? 0 : custom > 10 ? 10 : custom;
    pthread_mutex_unlock(&lock);
    if (changed)
        log_info("daynight: %s, IR LEDs %s%s", manual ? (leds ? "always night" : "always day") : "auto",
                 leds ? "on" : "off", !manual && lux ? ", custom level" : "");
}

void daynight_start(const struct config *c) {
    cfg = c;
    running = 1;
    pthread_create(&thread, NULL, daynight_main, NULL);
}

void daynight_stop(void) {
    if (!running)
        return;
    running = 0;
    pthread_join(thread, NULL);
}
