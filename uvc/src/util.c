#include "util.h"

#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <poll.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

int log_level = LOG_INFO;

void log_msg(int level, const char *fmt, ...) {
    static const char *names[] = {"E", "W", "I", "D"};
    if (level > log_level)
        return;
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    struct tm tm;
    localtime_r(&ts.tv_sec, &tm);
    char line[1024];
    int n = snprintf(line, sizeof(line), "%02d:%02d:%02d.%03ld %s ", tm.tm_hour, tm.tm_min,
                     tm.tm_sec, ts.tv_nsec / 1000000, names[level]);
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line + n, sizeof(line) - n, fmt, ap);
    va_end(ap);
    fprintf(stderr, "%s\n", line);
}

int64_t mono_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

int64_t wall_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

void sleep_ms(int ms) {
    struct timespec ts = {ms / 1000, (long)(ms % 1000) * 1000000};
    while (nanosleep(&ts, &ts) < 0 && errno == EINTR) {
    }
}

void buf_reserve(struct buf *b, size_t extra) {
    if (b->len + extra <= b->cap)
        return;
    size_t cap = b->cap ? b->cap : 256;
    while (cap < b->len + extra)
        cap *= 2;
    uint8_t *p = realloc(b->p, cap);
    if (!p) {
        log_error("out of memory (%zu bytes)", cap);
        abort();
    }
    b->p = p;
    b->cap = cap;
}

void buf_put(struct buf *b, const void *data, size_t len) {
    buf_reserve(b, len);
    memcpy(b->p + b->len, data, len);
    b->len += len;
}

void buf_u8(struct buf *b, uint8_t v) { buf_put(b, &v, 1); }

void buf_u16(struct buf *b, uint16_t v) {
    uint8_t p[2] = {v >> 8, v};
    buf_put(b, p, 2);
}

void buf_u24(struct buf *b, uint32_t v) {
    uint8_t p[3];
    put_u24(p, v);
    buf_put(b, p, 3);
}

void buf_u32(struct buf *b, uint32_t v) {
    uint8_t p[4];
    put_u32(p, v);
    buf_put(b, p, 4);
}

void buf_f64(struct buf *b, double v) {
    uint64_t u;
    memcpy(&u, &v, 8);
    buf_u32(b, u >> 32);
    buf_u32(b, (uint32_t)u);
}

void buf_consume(struct buf *b, size_t n) {
    if (n >= b->len) {
        b->len = 0;
        return;
    }
    memmove(b->p, b->p + n, b->len - n);
    b->len -= n;
}

void buf_free(struct buf *b) {
    free(b->p);
    b->p = NULL;
    b->len = b->cap = 0;
}

void put_u24(uint8_t *p, uint32_t v) {
    p[0] = v >> 16;
    p[1] = v >> 8;
    p[2] = v;
}

void put_u32(uint8_t *p, uint32_t v) {
    p[0] = v >> 24;
    p[1] = v >> 16;
    p[2] = v >> 8;
    p[3] = v;
}

void base64_encode(const uint8_t *in, size_t len, char *out) {
    static const char tbl[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t i;
    for (i = 0; i + 2 < len; i += 3) {
        uint32_t v = in[i] << 16 | in[i + 1] << 8 | in[i + 2];
        *out++ = tbl[v >> 18];
        *out++ = tbl[(v >> 12) & 63];
        *out++ = tbl[(v >> 6) & 63];
        *out++ = tbl[v & 63];
    }
    if (i < len) {
        uint32_t v = in[i] << 16 | (i + 1 < len ? in[i + 1] << 8 : 0);
        *out++ = tbl[v >> 18];
        *out++ = tbl[(v >> 12) & 63];
        *out++ = i + 1 < len ? tbl[(v >> 6) & 63] : '=';
        *out++ = '=';
    }
    *out = 0;
}

int write_all(int fd, const void *data, size_t len) {
    const uint8_t *p = data;
    while (len) {
        ssize_t n = send(fd, p, len, 0);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        p += n;
        len -= n;
    }
    return 0;
}

// Blocking TCP connect with a timeout; the returned socket is blocking again.
int tcp_connect(const char *host, int port, int timeout_ms) {
    char portstr[8];
    snprintf(portstr, sizeof(portstr), "%d", port);
    struct addrinfo hints = {0}, *res;
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(host, portstr, &hints, &res) != 0) {
        log_warn("cannot resolve %s", host);
        return -1;
    }
    int fd = socket(res->ai_family, res->ai_socktype, 0);
    if (fd < 0) {
        freeaddrinfo(res);
        return -1;
    }
    int flags = fcntl(fd, F_GETFL);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    int rc = connect(fd, res->ai_addr, res->ai_addrlen);
    freeaddrinfo(res);
    if (rc < 0 && errno == EINPROGRESS) {
        struct pollfd pfd = {fd, POLLOUT, 0};
        int err = 0;
        socklen_t elen = sizeof(err);
        if (poll(&pfd, 1, timeout_ms) == 1 &&
            getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &elen) == 0 && err == 0)
            rc = 0;
    }
    if (rc < 0) {
        close(fd);
        return -1;
    }
    fcntl(fd, F_SETFL, flags);
    return fd;
}
