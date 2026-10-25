#include "board.h"

#include <errno.h>
#include <fcntl.h>
#ifdef __linux__
#include <linux/watchdog.h>
#else // host builds on macOS: no watchdog device to open anyway
#define WDIOC_KEEPALIVE 0
#define WDIOC_SETTIMEOUT 0
#define WDIOC_GETTIMEOUT 0
#endif
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include "util.h"

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static int wdt_fd = -1; // guarded by lock
static int (*wdt_healthy)(void);
static int wdt_interval_ms;

static void *wdt_main(void *arg) {
    for (;;) {
        sleep_ms(wdt_interval_ms);
        int ok = wdt_healthy();
        pthread_mutex_lock(&lock);
        if (wdt_fd < 0) {
            pthread_mutex_unlock(&lock);
            return NULL;
        }
        if (ok)
            ioctl(wdt_fd, WDIOC_KEEPALIVE, 0);
        pthread_mutex_unlock(&lock);
        if (!ok)
            log_warn("watchdog: unhealthy, not feeding it");
    }
}

void board_watchdog_start(int timeout_s, int (*healthy)(void)) {
    int fd = open("/dev/watchdog", O_WRONLY | O_CLOEXEC);
    if (fd < 0) {
        log_info("watchdog: %s, running without one", strerror(errno));
        return;
    }
    // The jz watchdog starts at 5 s on open; ask for more, use what we get.
    int t = timeout_s;
    ioctl(fd, WDIOC_SETTIMEOUT, &t);
    if (ioctl(fd, WDIOC_GETTIMEOUT, &t) < 0 || t <= 0)
        t = 5;
    ioctl(fd, WDIOC_KEEPALIVE, 0);
    wdt_healthy = healthy;
    wdt_interval_ms = t * 1000 / 4;
    pthread_mutex_lock(&lock);
    wdt_fd = fd;
    pthread_mutex_unlock(&lock);
    pthread_t th;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    pthread_create(&th, &attr, wdt_main, NULL);
    pthread_attr_destroy(&attr);
    log_info("watchdog: armed, %d s", t);
}

void board_watchdog_stop(void) {
    pthread_mutex_lock(&lock);
    if (wdt_fd >= 0) {
        if (write(wdt_fd, "V", 1) != 1)
            log_warn("watchdog: magic close failed");
        close(wdt_fd);
        wdt_fd = -1;
        log_info("watchdog: disarmed");
    }
    pthread_mutex_unlock(&lock);
}

static int led_enabled = 1, led_connected; // guarded by lock

static void led_write(const char *name, int on) {
    char path[64];
    snprintf(path, sizeof(path), "/sys/class/leds/%s/brightness", name);
    FILE *fp = fopen(path, "w");
    if (fp) {
        fputs(on ? "1" : "0", fp);
        fclose(fp);
    }
}

// lock held
static void led_update(void) {
    led_write("led_blue", led_enabled && led_connected);
    led_write("led_yellow", led_enabled && !led_connected);
}

void board_led_connected(int connected) {
    pthread_mutex_lock(&lock);
    led_connected = connected;
    led_update();
    pthread_mutex_unlock(&lock);
}

void board_led_enable(int enable) {
    pthread_mutex_lock(&lock);
    led_enabled = enable;
    led_update();
    pthread_mutex_unlock(&lock);
}
