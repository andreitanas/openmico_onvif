#include "source.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>

#ifdef __linux__
#include <linux/videodev2.h>
#endif

#include "h264.h"
#include "util.h"

#define MAX_FRAME (1024 * 1024)

void frame_ref(struct frame *f) { __atomic_add_fetch(&f->refs, 1, __ATOMIC_RELAXED); }

void frame_unref(struct frame *f) {
    if (f && __atomic_sub_fetch(&f->refs, 1, __ATOMIC_ACQ_REL) == 0)
        free(f);
}

void queue_init(struct queue *q) {
    memset(q, 0, sizeof(*q));
    pthread_mutex_init(&q->lock, NULL);
    pthread_cond_init(&q->cond, NULL);
    q->need_key = 1;
}

void queue_destroy(struct queue *q) {
    while (q->count) {
        frame_unref(q->items[q->head]);
        q->head = (q->head + 1) % QUEUE_CAP;
        q->count--;
    }
    pthread_mutex_destroy(&q->lock);
    pthread_cond_destroy(&q->cond);
}

static void queue_push(struct queue *q, struct frame *f) {
    pthread_mutex_lock(&q->lock);
    if (q->need_key && !f->key)
        goto out;
    if (q->count == QUEUE_CAP) {
        // Consumer is stuck (slow network): flush and resync on a keyframe.
        while (q->count) {
            frame_unref(q->items[q->head]);
            q->head = (q->head + 1) % QUEUE_CAP;
            q->count--;
        }
        q->need_key = 1;
        if (!f->key)
            goto out;
    }
    q->need_key = 0;
    frame_ref(f);
    q->items[(q->head + q->count) % QUEUE_CAP] = f;
    q->count++;
    pthread_cond_signal(&q->cond);
out:
    pthread_mutex_unlock(&q->lock);
}

struct frame *queue_pop(struct queue *q, int timeout_ms) {
    struct frame *f = NULL;
    pthread_mutex_lock(&q->lock);
    if (!q->count) {
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_sec += timeout_ms / 1000;
        ts.tv_nsec += (long)(timeout_ms % 1000) * 1000000;
        if (ts.tv_nsec >= 1000000000) {
            ts.tv_sec++;
            ts.tv_nsec -= 1000000000;
        }
        while (!q->count && pthread_cond_timedwait(&q->cond, &q->lock, &ts) == 0) {
        }
    }
    if (q->count) {
        f = q->items[q->head];
        q->head = (q->head + 1) % QUEUE_CAP;
        q->count--;
    }
    pthread_mutex_unlock(&q->lock);
    return f;
}

struct frame *frame_new(size_t len) {
    struct frame *f = malloc(sizeof(*f) + len);
    if (!f)
        return NULL;
    f->refs = 1;
    f->key = 0;
    f->ts_ms = mono_ms();
    f->len = len;
    return f;
}

void source_push(struct source *s, struct frame *f) {
    f->key = h264_is_key(f->data, f->len);
    pthread_mutex_lock(&s->lock);
    s->last_frame_ms = f->ts_ms;
    if (f->key && !s->width) {
        size_t pos = 0, n;
        const uint8_t *nal;
        while (h264_next_nal(f->data, f->len, &pos, &nal, &n)) {
            int w, h;
            if (n && h264_type(nal) == NAL_SPS && h264_sps_size(nal, n, &w, &h) == 0) {
                s->width = w;
                s->height = h;
                log_info("%s: %dx%d H.264", s->path, w, h);
                break;
            }
        }
    }
    for (struct queue *q = s->subs; q; q = q->next)
        queue_push(q, f);
    pthread_mutex_unlock(&s->lock);
}

static void publish(struct source *s, const uint8_t *data, size_t len) {
    struct frame *f = frame_new(len);
    if (!f)
        return;
    memcpy(f->data, data, len);
    source_push(s, f);
    frame_unref(f);
}

// v4l2loopback hands out exactly one encoder frame per read(): OpenMiko's
// videocapture writes all NAL packs of a frame in a single write().
static void read_device(struct source *s, uint8_t *buf) {
    int fd = open(s->path, O_RDONLY);
    if (fd < 0) {
        log_warn("%s: %s", s->path, strerror(errno));
        sleep_ms(2000);
        return;
    }
#ifdef __linux__
    struct v4l2_format fmt = {0};
    fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (ioctl(fd, VIDIOC_G_FMT, &fmt) == 0)
        log_debug("%s: sizeimage %u", s->path, fmt.fmt.pix.sizeimage);
#endif
    for (;;) {
        ssize_t n = read(fd, buf, MAX_FRAME);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0) {
            log_warn("%s: read failed: %s", s->path, n < 0 ? strerror(errno) : "EOF");
            break;
        }
        publish(s, buf, n);
    }
    close(fd);
    sleep_ms(1000);
}

// For testing without a camera: split an Annex-B file into access units and
// replay it in a loop at the configured frame rate. A new access unit starts
// at an AUD/SPS/PPS/SEI after a slice, or at a slice whose first_mb is 0.
static void read_file(struct source *s, uint8_t *buf) {
    FILE *fp = fopen(s->path, "rb");
    if (!fp) {
        log_error("%s: %s", s->path, strerror(errno));
        sleep_ms(2000);
        return;
    }
    size_t size = fread(buf, 1, MAX_FRAME * 8, fp);
    fclose(fp);

    int64_t period = 1000 / (s->fps > 0 ? s->fps : 25);
    int64_t next = mono_ms();
    for (;;) {
        size_t pos = 0, au_start = SIZE_MAX, n;
        int seen_slice = 0;
        const uint8_t *nal;
        while (h264_next_nal(buf, size, &pos, &nal, &n)) {
            if (!n)
                continue;
            int t = h264_type(nal);
            int slice = t == NAL_SLICE || t == NAL_IDR;
            int boundary = (slice && n > 1 && (nal[1] & 0x80)) ||
                           (!slice && (t == NAL_AUD || t == NAL_SPS || t == NAL_PPS || t == NAL_SEI));
            // Back up over the start code that precedes this NAL.
            size_t nal_off = nal - buf;
            size_t sc = nal_off >= 4 && buf[nal_off - 4] == 0 ? 4 : 3;
            if (boundary && seen_slice && au_start != SIZE_MAX) {
                sleep_ms((int)(next - mono_ms() > 0 ? next - mono_ms() : 0));
                next += period;
                publish(s, buf + au_start, nal_off - sc - au_start);
                au_start = nal_off - sc;
                seen_slice = 0;
            }
            if (au_start == SIZE_MAX)
                au_start = nal_off - sc;
            if (slice)
                seen_slice = 1;
        }
        if (au_start != SIZE_MAX && seen_slice) {
            sleep_ms((int)(next - mono_ms() > 0 ? next - mono_ms() : 0));
            next += period;
            publish(s, buf + au_start, size - au_start);
        }
    }
}

static void *source_main(void *arg) {
    struct source *s = arg;
    uint8_t *buf = malloc(s->file ? MAX_FRAME * 8 : MAX_FRAME);
    if (!buf)
        return NULL;
    for (;;) {
        if (s->file)
            read_file(s, buf);
        else
            read_device(s, buf);
    }
    return NULL;
}

void source_init(struct source *s, const char *name) {
    memset(s, 0, sizeof(*s));
    snprintf(s->path, sizeof(s->path), "%s", name);
    pthread_mutex_init(&s->lock, NULL);
}

int source_start(struct source *s, const char *path, int fps) {
    source_init(s, path);
    s->fps = fps;
    struct stat st;
    s->file = stat(path, &st) == 0 && S_ISREG(st.st_mode);
    return pthread_create(&s->thread, NULL, source_main, s);
}

void source_subscribe(struct source *s, struct queue *q) {
    pthread_mutex_lock(&s->lock);
    q->next = s->subs;
    s->subs = q;
    pthread_mutex_unlock(&s->lock);
}

void source_unsubscribe(struct source *s, struct queue *q) {
    pthread_mutex_lock(&s->lock);
    for (struct queue **p = &s->subs; *p; p = &(*p)->next) {
        if (*p == q) {
            *p = q->next;
            break;
        }
    }
    pthread_mutex_unlock(&s->lock);
}

int source_wait_ready(struct source *s, int timeout_ms) {
    int64_t end = mono_ms() + timeout_ms;
    while (mono_ms() < end) {
        pthread_mutex_lock(&s->lock);
        int ready = s->width > 0;
        pthread_mutex_unlock(&s->lock);
        if (ready)
            return 0;
        sleep_ms(100);
    }
    return -1;
}

int read_jpeg(const char *path, uint8_t **data, size_t *len) {
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return -1;
    size_t cap = 512 * 1024;
    uint8_t *p = malloc(cap);
    ssize_t n = p ? read(fd, p, cap) : -1;
    close(fd);
    // A device read returns one whole JPEG; a file is read up to cap.
    if (n < 4 || p[0] != 0xff || p[1] != 0xd8) {
        free(p);
        return -1;
    }
    *data = p;
    *len = n;
    return 0;
}
