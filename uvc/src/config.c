#include "config.h"

#include <arpa/inet.h>
#include <ctype.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "util.h"

#define SET(field, val) snprintf(c->field, sizeof(c->field), "%s", val)

static void defaults(struct config *c) {
    memset(c, 0, sizeof(*c));
    // The camera image keeps settings in the config flash partition and
    // uvcd with its model in the read-only root partition (see image/).
    SET(cert, "/config/uvc/client.pem");
    SET(iface, "wlan0");
    // UVC G5 Pro: an identity known to adopt with smart detections on
    // Protect 7.x. FW_VERSION should match the newest firmware Protect's
    // catalog has for this platform, or Protect nags/forces updates.
    SET(model, "UVC G5 Pro");
    SET(sysid, "0xa598");
    SET(platform, "sav837gw");
    SET(fw_version, "5.5.31");
    SET(fw_build, "92ebf37.260724.1639");
    SET(hi_source, "/dev/video3");
    SET(snapshot, "/dev/video4");
    SET(state_file, "/config/uvc/uvcd.state");
    // Measured on a jxf22 Wyze Cam v2 (see daynight.h): day mode reads
    // ~450k in a lit room and 1.2M+ covered; night mode without IR LEDs
    // 0.9M+ covered.
    c->night_exposure = 1200000;
    c->day_exposure = 800000;
    c->watchdog = 60;
    SET(detect_model, "/rom/uvc/models/yolo-fastest-1.1");
    c->capture = 1;
    c->hi_kbps = 2000;
    c->lo_kbps = 300;
    c->hi_gop = 50;
    c->lo_gop = 25;
    c->lo_width = 640;
    c->lo_height = 360;
    c->fps = 25;
    c->audio = 1;
    c->allow_reboot = 1;
}

static char *trim(char *s) {
    while (isspace((unsigned char)*s))
        s++;
    char *e = s + strlen(s);
    while (e > s && isspace((unsigned char)e[-1]))
        *--e = 0;
    if (e - s >= 2 && (*s == '"' || *s == '\'') && e[-1] == *s) {
        e[-1] = 0;
        s++;
    }
    return s;
}

static void set_key(struct config *c, const char *k, const char *v) {
    if (!strcmp(k, "NVR_HOST")) SET(nvr, v);
    else if (!strcmp(k, "ADOPTION_TOKEN")) SET(token, v);
    else if (!strcmp(k, "CERT")) SET(cert, v);
    else if (!strcmp(k, "NAME")) SET(name, v);
    else if (!strcmp(k, "IFACE")) SET(iface, v);
    else if (!strcmp(k, "MAC")) SET(mac, v);
    else if (!strcmp(k, "IP")) SET(ip, v);
    else if (!strcmp(k, "MODEL")) SET(model, v);
    else if (!strcmp(k, "SYSID")) SET(sysid, v);
    else if (!strcmp(k, "PLATFORM")) SET(platform, v);
    else if (!strcmp(k, "FW_VERSION")) SET(fw_version, v);
    else if (!strcmp(k, "FW_BUILD")) SET(fw_build, v);
    else if (!strcmp(k, "HI_SOURCE")) SET(hi_source, v);
    else if (!strcmp(k, "LO_SOURCE")) SET(lo_source, v);
    else if (!strcmp(k, "SNAPSHOT_SOURCE")) SET(snapshot, v);
    else if (!strcmp(k, "STATE_FILE")) SET(state_file, v);
    else if (!strcmp(k, "LOG_FILE")) SET(log_file, v);
    else if (!strcmp(k, "CAPTURE")) c->capture = atoi(v);
    else if (!strcmp(k, "HI_BITRATE")) c->hi_kbps = atoi(v);
    else if (!strcmp(k, "LO_BITRATE")) c->lo_kbps = atoi(v);
    else if (!strcmp(k, "HI_GOP")) c->hi_gop = atoi(v);
    else if (!strcmp(k, "LO_GOP")) c->lo_gop = atoi(v);
    else if (!strcmp(k, "LO_WIDTH")) c->lo_width = atoi(v);
    else if (!strcmp(k, "LO_HEIGHT")) c->lo_height = atoi(v);
    else if (!strcmp(k, "FLIP_V")) c->flip_v = atoi(v);
    else if (!strcmp(k, "FLIP_H")) c->flip_h = atoi(v);
    else if (!strcmp(k, "NIGHT_EXPOSURE")) c->night_exposure = atoi(v);
    else if (!strcmp(k, "DAY_EXPOSURE")) c->day_exposure = atoi(v);
    else if (!strcmp(k, "WATCHDOG")) c->watchdog = atoi(v);
    else if (!strcmp(k, "DETECT_MODEL")) SET(detect_model, v);
    else if (!strcmp(k, "FPS")) c->fps = atoi(v);
    else if (!strcmp(k, "AUDIO")) c->audio = atoi(v);
    else if (!strcmp(k, "ALLOW_REBOOT")) c->allow_reboot = atoi(v);
    else if (!strcmp(k, "ADOPTED")) c->adopted = atoi(v);
    else if (!strcmp(k, "LOG_LEVEL")) log_level = atoi(v);
    else log_warn("unknown config key %s", k);
}

// Shell-style KEY=value lines, like openmiko.conf.
static int parse_file(struct config *c, const char *path, int required) {
    FILE *fp = fopen(path, "r");
    if (!fp) {
        if (required)
            log_error("cannot open %s", path);
        return required ? -1 : 0;
    }
    char line[512];
    while (fgets(line, sizeof(line), fp)) {
        char *s = trim(line);
        if (!*s || *s == '#')
            continue;
        char *eq = strchr(s, '=');
        if (!eq)
            continue;
        *eq = 0;
        set_key(c, trim(s), trim(eq + 1));
    }
    fclose(fp);
    return 0;
}

static void normalize_mac(char *mac) {
    char out[13];
    int n = 0;
    for (char *p = mac; *p && n < 12; p++)
        if (isxdigit((unsigned char)*p))
            out[n++] = toupper((unsigned char)*p);
    out[n] = 0;
    strcpy(mac, out);
}

static void discover(struct config *c) {
    if (!*c->mac) {
        char path[64];
        snprintf(path, sizeof(path), "/sys/class/net/%s/address", c->iface);
        FILE *fp = fopen(path, "r");
        if (fp) {
            char line[32] = {0};
            if (fgets(line, sizeof(line), fp))
                SET(mac, line);
            fclose(fp);
        }
    }
    normalize_mac(c->mac);
    if (!*c->ip) {
        struct ifaddrs *ifs, *i;
        if (getifaddrs(&ifs) == 0) {
            for (i = ifs; i; i = i->ifa_next) {
                if (!i->ifa_addr || i->ifa_addr->sa_family != AF_INET || (i->ifa_flags & IFF_LOOPBACK))
                    continue;
                if (strcmp(i->ifa_name, c->iface) && *c->ip)
                    continue;
                inet_ntop(AF_INET, &((struct sockaddr_in *)i->ifa_addr)->sin_addr, c->ip,
                          sizeof(c->ip));
                if (!strcmp(i->ifa_name, c->iface))
                    break;
            }
            freeifaddrs(ifs);
        }
    }
    if (!*c->name)
        gethostname(c->name, sizeof(c->name) - 1);
    if (!*c->lo_source)
        SET(lo_source, c->hi_source);
}

// Why Protect cannot be used (yet), or NULL.
static const char *protect_missing(const struct config *c) {
    if (!*c->nvr || strlen(c->mac) != 12 || !*c->ip)
        return "config needs NVR_HOST, and a MAC/IP (from IFACE or MAC=/IP=)";
    if (!c->adopted && !*c->token)
        return "not adopted yet: set ADOPTION_TOKEN (Protect: /proxy/protect/api/cameras/manage-payload)";
    return NULL;
}

int config_load(struct config *c, const char *path) {
    defaults(c);
    if (parse_file(c, path, 1) < 0)
        return -1;
    discover(c);
    // The state file only ever records that adoption succeeded.
    parse_file(c, c->state_file, 0);
    // Without Protect settings a capturing uvcd still owns the camera (motion
    // detection, the watchdog) until it is configured.
    const char *missing = protect_missing(c);
    c->protect = !missing;
    if (missing) {
        if (!c->capture) {
            log_error("%s", missing);
            return -1;
        }
        log_warn("%s; capturing only", missing);
    }
    return 0;
}

int config_refresh(struct config *c) {
    if (!*c->ip)
        discover(c);
    c->protect = !protect_missing(c);
    return c->protect;
}

int config_save_adopted(const struct config *c) {
    FILE *fp = fopen(c->state_file, "w");
    if (!fp) {
        log_warn("cannot write %s", c->state_file);
        return -1;
    }
    fprintf(fp, "ADOPTED=1\n");
    fclose(fp);
    return 0;
}
