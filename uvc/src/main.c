// uvcd: makes an OpenMiko camera adopt into UniFi Protect as a native
// UniFi camera, speaking Protect's own camera protocol on the device.

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "avclient.h"
#include "config.h"
#include "source.h"
#include "tls.h"
#include "util.h"

static void usage(void) {
    fprintf(stderr, "usage: uvcd [-c /etc/uvcd.conf] [-v]\n");
    exit(2);
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

    if (tls_init(cfg.cert) < 0)
        return 1;

    static struct source hi, lo;
    struct source *lop = &hi;
    source_start(&hi, cfg.hi_source, cfg.fps);
    if (strcmp(cfg.lo_source, cfg.hi_source)) {
        source_start(&lo, cfg.lo_source, cfg.fps);
        lop = &lo;
    }
    // Resolutions go into the adoption handshake, so learn them first.
    while (source_wait_ready(&hi, 10000) < 0 || source_wait_ready(lop, 10000) < 0)
        log_warn("waiting for H.264 from %s / %s", cfg.hi_source, cfg.lo_source);

    avclient_run(&cfg, &hi, lop);
    return 0;
}
