// uvcd: makes a Wyze Cam v2 (Ingenic T20) adopt into UniFi Protect as a native
// UniFi camera, speaking Protect's own camera protocol on the device.

#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "analytics.h"
#include "avclient.h"
#include "board.h"
#include "capture.h"
#include "config.h"
#include "https.h"
#include "isp.h"
#include "objects.h"
#include "source.h"
#include "tls.h"
#include "util.h"

#define SANE_WALL_MS 1704067200000LL // 2024-01-01

static struct config cfg;
static struct source hi, lo;
static int64_t start_ms;

// Fed to the hardware watchdog: video keeps coming, and Protect has been
// reachable within the last half hour (a wedged WiFi driver is not
// otherwise recovered from).
static int healthy(void) {
    int64_t now = mono_ms();
    pthread_mutex_lock(&hi.lock);
    int64_t last = hi.last_frame_ms;
    pthread_mutex_unlock(&hi.lock);
    if (now - last > 15000) {
        log_warn("no video for %lld s", (long long)(now - last) / 1000);
        return 0;
    }
    int64_t seen = avclient_connected_ms();
    if (seen < start_ms)
        seen = start_ms;
    if (cfg.protect && now - seen > 30 * 60000) {
        log_warn("no connection to Protect for %lld min", (long long)(now - seen) / 60000);
        return 0;
    }
    return 1;
}

// Protect places video and events by our wall clock. The camera has no RTC
// and boots in 1970 until NTP steps the clock. Without NTP (a camera network
// with no internet access) take the time from Protect's web server.
static void wait_for_clock(void) {
    if (wall_ms() >= SANE_WALL_MS)
        return;
    log_info("waiting for the clock to be set (NTP)");
    int64_t next_try = mono_ms() + 30000;
    while (wall_ms() < SANE_WALL_MS) {
        int64_t ms;
        if (mono_ms() >= next_try) {
            next_try = mono_ms() + 10000;
            if (https_date(cfg.nvr, 443, &ms) == 0 && ms >= SANE_WALL_MS) {
                struct timespec ts = {ms / 1000, 0};
                if (clock_settime(CLOCK_REALTIME, &ts) == 0) {
                    log_warn("no NTP; clock set from %s", cfg.nvr);
                    return;
                }
            }
        }
        sleep_ms(500);
    }
    log_info("clock set");
}

static void usage(void) {
    fprintf(stderr, "usage: uvcd [-c /config/uvc/uvcd.conf] [-v]\n");
    exit(2);
}

// SIGTERM/SIGINT are blocked in every thread and taken here, so Protect
// sees a proper disconnect and libimp is shut down cleanly for whoever owns
// the camera next.
static void *signal_main(void *arg) {
    sigset_t *set = arg;
    int sig;
    sigwait(set, &sig);
    log_info("signal %d: stopping", sig);
    board_watchdog_stop();
    avclient_stop(3000);
    capture_stop();
    _exit(0);
}

int main(int argc, char **argv) {
    const char *conf = "/config/uvc/uvcd.conf";
    int verbose = 0, opt;
    while ((opt = getopt(argc, argv, "c:vh")) != -1) {
        if (opt == 'c')
            conf = optarg;
        else if (opt == 'v')
            verbose = 1;
        else
            usage();
    }
    signal(SIGPIPE, SIG_IGN);

    start_ms = mono_ms();
    if (config_load(&cfg, conf) < 0)
        return 1;
    if (verbose)
        log_level = LOG_DEBUG;
    if (*cfg.log_file)
        log_open(cfg.log_file, 256 * 1024);
    log_info("uvcd: %s mac %s ip %s, Protect %s", cfg.name, cfg.mac, cfg.ip, cfg.nvr);

    static sigset_t stop_set;
    sigemptyset(&stop_set);
    sigaddset(&stop_set, SIGTERM);
    sigaddset(&stop_set, SIGINT);
    pthread_sigmask(SIG_BLOCK, &stop_set, NULL);
    pthread_t sig_thread;
    pthread_create(&sig_thread, NULL, signal_main, &stop_set);

    board_led_connected(0);
    isp_init(cfg.ir_script);
    struct source *lop = &hi;
    if (cfg.capture) {
        source_init(&hi, "capture:hi");
        source_init(&lo, "capture:lo");
        lop = &lo;
        if (capture_start(&cfg, &hi, &lo) < 0)
            return 1;
        analytics_start(avclient_event, cfg.lo_width, cfg.lo_height);
        if (*cfg.detect_model)
            objects_start(avclient_event, cfg.detect_model, cfg.lo_width, cfg.lo_height);
    } else {
        source_start(&hi, cfg.hi_source, cfg.fps);
        if (strcmp(cfg.lo_source, cfg.hi_source)) {
            source_start(&lo, cfg.lo_source, cfg.fps);
            lop = &lo;
        }
    }
    // Resolutions go into the adoption handshake, so learn them first.
    while (source_wait_ready(&hi, 10000) < 0 || source_wait_ready(lop, 10000) < 0)
        log_warn("waiting for H.264 from %s / %s", hi.path, lop->path);

    if (cfg.capture) {
        if (cfg.watchdog > 0)
            board_watchdog_start(cfg.watchdog, healthy);
    }

    if (!cfg.protect) {
        while (!config_refresh(&cfg))
            sleep_ms(5000);
        log_info("uvcd: ip %s, Protect %s", cfg.ip, cfg.nvr);
    }
    if (tls_init(cfg.cert) < 0)
        return 1;
    wait_for_clock();
    avclient_run(&cfg, &hi, lop);
    return 0;
}
