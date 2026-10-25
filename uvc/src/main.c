// uvcd: makes an OpenMiko camera adopt into UniFi Protect as a native
// UniFi camera, speaking Protect's own camera protocol on the device.

#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "analytics.h"
#include "avclient.h"
#include "capture.h"
#include "config.h"
#include "objects.h"
#include "source.h"
#include "tls.h"
#include "util.h"

#define READY_FILE "/var/run/uvcd.ready"

static void usage(void) {
    fprintf(stderr, "usage: uvcd [-c /etc/uvcd.conf] [-v]\n");
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
    avclient_stop(3000);
    capture_stop();
    _exit(0);
}

int main(int argc, char **argv) {
    const char *conf = "/etc/uvcd.conf";
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

    static struct config cfg;
    if (config_load(&cfg, conf) < 0)
        return 1;
    if (verbose)
        log_level = LOG_DEBUG;
    log_info("uvcd: %s mac %s ip %s, Protect %s", cfg.name, cfg.mac, cfg.ip, cfg.nvr);

    static sigset_t stop_set;
    sigemptyset(&stop_set);
    sigaddset(&stop_set, SIGTERM);
    sigaddset(&stop_set, SIGINT);
    pthread_sigmask(SIG_BLOCK, &stop_set, NULL);
    pthread_t sig_thread;
    pthread_create(&sig_thread, NULL, signal_main, &stop_set);

    static struct source hi, lo;
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
        // Tells S60camera the v4l2loopback devices are live for the RTSP server.
        FILE *fp = fopen(READY_FILE, "w");
        if (fp)
            fclose(fp);
    }

    if (!cfg.protect) {
        while (!config_refresh(&cfg))
            sleep_ms(5000);
        log_info("uvcd: ip %s, Protect %s", cfg.ip, cfg.nvr);
    }
    if (tls_init(cfg.cert) < 0)
        return 1;
    avclient_run(&cfg, &hi, lop);
    return 0;
}
