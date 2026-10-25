#include "stream.h"

#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include "flv.h"
#include "h264.h"
#include "util.h"

static int send_buf(int fd, struct buf *b) {
    if (b->len && write_all(fd, b->p, b->len) < 0)
        return -1;
    b->len = 0;
    return 0;
}

// Protect answers on the stream connection in a small binary protocol (a
// 27-byte greeting, then 16-byte reports about once a second that appear to
// carry the received byte rate). Not needed yet; drained and logged
// (printable bytes as text, the rest as hex) to learn the format.
static void log_replies(struct stream *s, int fd) {
    uint8_t in[256];
    ssize_t n = recv(fd, in, sizeof(in), MSG_DONTWAIT);
    if (n <= 0)
        return;
    char text[sizeof(in) * 4 + 1];
    size_t k = 0;
    for (ssize_t i = 0; i < n; i++)
        k += snprintf(text + k, sizeof(text) - k, in[i] >= 0x20 && in[i] < 0x7f ? "%c" : "\\x%02x", in[i]);
    log_debug("%s: Protect sent %zd bytes: %s", s->channel, n, text);
}

// Sends one connection's worth of stream; returns when the socket fails or
// the stream is stopped.
static void session(struct stream *s, int fd) {
    struct flv flv;
    int started = 0;
    int64_t first_ts = 0;

    queue_init(&s->q);
    source_subscribe(s->src, &s->q);
    while (!s->stop) {
        struct frame *f = queue_pop(&s->q, 500);
        if (!f) {
            if (started && mono_ms() - s->src->last_frame_ms > 10000) {
                log_warn("%s: no frames from %s for 10 s", s->channel, s->src->path);
                break;
            }
            continue;
        }
        if (!started) {
            // The first frame is always a keyframe (queue_push guarantees
            // it); Ingenic keyframes carry SPS/PPS in band.
            const uint8_t *sps = NULL, *pps = NULL, *nal;
            size_t sps_len = 0, pps_len = 0, pos = 0, n;
            while (h264_next_nal(f->data, f->len, &pos, &nal, &n)) {
                if (n && h264_type(nal) == NAL_SPS)
                    sps = nal, sps_len = n;
                else if (n && h264_type(nal) == NAL_PPS)
                    pps = nal, pps_len = n;
            }
            if (!sps || !pps) {
                frame_unref(f);
                continue;
            }
            flv_begin(&flv, s->name, s->src->width, s->src->height, s->fps, s->audio);
            flv_avc_config(&flv, sps, sps_len, pps, pps_len);
            first_ts = f->ts_ms;
            started = 1;
            log_info("%s: streaming %s to %s:%d", s->channel, s->name, s->host, s->port);
        }
        flv_video(&flv, (uint32_t)(f->ts_ms - first_ts), f->key, f->data, f->len);
        frame_unref(f);
        if (send_buf(fd, &flv.out) < 0) {
            log_warn("%s: connection to %s:%d lost", s->channel, s->host, s->port);
            break;
        }
        log_replies(s, fd);
    }
    source_unsubscribe(s->src, &s->q);
    queue_destroy(&s->q);
    if (started)
        flv_free(&flv);
}

static void *stream_main(void *arg) {
    struct stream *s = arg;
    int backoff = 500;
    while (!s->stop) {
        int fd = tcp_connect(s->host, s->port, 5000);
        if (fd < 0) {
            log_warn("%s: cannot connect to %s:%d", s->channel, s->host, s->port);
        } else {
            struct timeval tv = {5, 0};
            setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
            int64_t t0 = mono_ms();
            session(s, fd);
            close(fd);
            if (mono_ms() - t0 > 30000)
                backoff = 500;
        }
        for (int waited = 0; waited < backoff && !s->stop; waited += 100)
            sleep_ms(100);
        backoff = backoff < 8000 ? backoff * 2 : 8000;
    }
    return NULL;
}

void stream_start(struct stream *s, const char *name, const char *host, int port) {
    if (s->running)
        stream_stop(s);
    snprintf(s->name, sizeof(s->name), "%s", name);
    snprintf(s->host, sizeof(s->host), "%s", host);
    s->port = port;
    s->stop = 0;
    s->running = pthread_create(&s->thread, NULL, stream_main, s) == 0;
}

void stream_stop(struct stream *s) {
    if (!s->running)
        return;
    log_info("%s: stopping", s->channel);
    s->stop = 1;
    pthread_join(s->thread, NULL);
    s->running = 0;
}

int stream_matches(const struct stream *s, const char *name, const char *host, int port) {
    return s->running && !strcmp(s->name, name) && !strcmp(s->host, host) && s->port == port;
}
