#include "discovery.h"

#include <arpa/inet.h>
#include <ctype.h>
#include <ifaddrs.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include "util.h"

#define PORT 10001
#define MCAST "233.89.188.1"

// Fields of a discovery reply: type, 16-bit big-endian length, value.
enum {
    HWADDR = 0x01,
    IPINFO = 0x02, // MAC + IPv4
    FWVERSION = 0x03,
    UPTIME = 0x0a,
    HOSTNAME = 0x0b,
    PLATFORM = 0x0c, // the model name, e.g. "UVC G4 Instant"
    MGMT_IS_DEFAULT = 0x17, // 1: not adopted
    SYSTEM_ID = 0x10, // 16-bit little-endian
    DEVICE_ID = 0x20,
    DEFAULT_CREDENTIALS = 0x2c,
    CAPABILITIES = 0x3b,
};

static const struct config *cfg;

static void put(struct buf *b, int type, const void *data, size_t len) {
    buf_u8(b, type);
    buf_u16(b, len);
    buf_put(b, data, len);
}

static void put_str(struct buf *b, int type, const char *s) { put(b, type, s, strlen(s)); }

// The interface's IPv4 address now (DHCP may have changed it).
static int iface_ip(uint8_t ip[4]) {
    struct ifaddrs *ifs, *i;
    int rc = -1;
    if (getifaddrs(&ifs))
        return -1;
    for (i = ifs; i; i = i->ifa_next) {
        if (i->ifa_addr && i->ifa_addr->sa_family == AF_INET && !strcmp(i->ifa_name, cfg->iface)) {
            memcpy(ip, &((struct sockaddr_in *)i->ifa_addr)->sin_addr, 4);
            rc = 0;
            break;
        }
    }
    freeifaddrs(ifs);
    return rc;
}

// What a real camera sends (captured from a G4 Instant and G6 Turrets on
// 5.5.31), with our identity.
static int reply(struct buf *b) {
    uint8_t mac[6], ip[4];
    if (strlen(cfg->mac) != 12 || iface_ip(ip))
        return -1;
    for (int i = 0; i < 6; i++) {
        char h[3] = {cfg->mac[2 * i], cfg->mac[2 * i + 1], 0};
        mac[i] = strtol(h, NULL, 16);
    }
    uint8_t macip[10];
    memcpy(macip, mac, 6);
    memcpy(macip + 6, ip, 4);
    // Protect reads the version from "UVC.<platform>.v<x.y.z>.<build>", and
    // talks HTTPS to cameras from 4.9.2 on.
    char fw[128], platform[32];
    snprintf(platform, sizeof(platform), "%s", cfg->platform);
    for (char *p = platform; *p; p++)
        *p = toupper((unsigned char)*p);
    snprintf(fw, sizeof(fw), "UVC.%s.v%s.%s", platform, cfg->fw_version, cfg->fw_build);
    uint8_t up[4], sysid[2], managed = !cfg->adopted, creds = 3, caps[4] = {0};
    put_u32(up, (uint32_t)(mono_ms() / 1000));
    long id = strtol(cfg->sysid, NULL, 0);
    sysid[0] = id & 0xff;
    sysid[1] = id >> 8;

    b->len = 0;
    buf_u8(b, 1); // version
    buf_u8(b, 0); // info
    buf_u16(b, 0); // length, below
    put(b, IPINFO, macip, 10);
    put(b, HWADDR, mac, 6);
    put(b, UPTIME, up, 4);
    put_str(b, HOSTNAME, cfg->name);
    put_str(b, PLATFORM, cfg->model);
    put(b, MGMT_IS_DEFAULT, &managed, 1);
    put_str(b, FWVERSION, fw);
    put(b, SYSTEM_ID, sysid, 2);
    put_str(b, DEVICE_ID, cfg->device_id);
    put(b, DEFAULT_CREDENTIALS, &creds, 1);
    put(b, CAPABILITIES, caps, 4);
    b->p[2] = (b->len - 4) >> 8;
    b->p[3] = (b->len - 4) & 0xff;
    return 0;
}

static void *discovery_main(void *arg) {
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    int on = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
    struct sockaddr_in addr = {.sin_family = AF_INET, .sin_port = htons(PORT)};
    while (bind(fd, (struct sockaddr *)&addr, sizeof(addr))) {
        log_warn("discovery: cannot bind port %d, retrying", PORT);
        sleep_ms(30000);
    }
    struct timeval tv = {5, 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    struct ip_mreq mreq = {.imr_multiaddr.s_addr = inet_addr(MCAST)};
    int joined = 0;
    struct buf b = {0};
    for (;;) {
        // Protect probes broadcast and multicast alike; the group can only
        // be joined once the interface is up.
        if (!joined && !setsockopt(fd, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof(mreq)))
            joined = 1;
        uint8_t probe[64];
        struct sockaddr_in from;
        socklen_t from_len = sizeof(from);
        ssize_t n = recvfrom(fd, probe, sizeof(probe), 0, (struct sockaddr *)&from, &from_len);
        // A probe is a bare header: version 1 or 2, command 0, length 0.
        if (n != 4 || (probe[0] != 1 && probe[0] != 2) || probe[2] || probe[3])
            continue;
        if (reply(&b) == 0) {
            sendto(fd, b.p, b.len, 0, (struct sockaddr *)&from, from_len);
            log_debug("discovery: answered %s", inet_ntoa(from.sin_addr));
        }
    }
    return NULL;
}

void discovery_start(const struct config *c) {
    cfg = c;
    pthread_t thread;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    pthread_create(&thread, &attr, discovery_main, NULL);
    pthread_attr_destroy(&attr);
}
