// The camera pipeline on our kernel (docs/kernel-modern.md): the same
// interface as capture.c's libimp one, over the V4L2 drivers. The ISP's two
// outputs (t20-isp-main at the sensor's size, t20-isp-sub scaled) go as
// dma-bufs to one H.264 encoder context each (t20-h264), the main one also
// to the JPEG encoder (t20-jpeg) on demand. Auto exposure runs here, on the
// low-resolution frames, through the sensor's exposure and gain controls.

#include "capture_v4l2.h"

#include "util.h"

#if defined(__linux__)

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/videodev2.h>
#include <math.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#define ISP_BUFS 3  // per ISP output, shared with the encoders as dma-bufs
#define ENC_BUFS 2  // encoded frames in flight per encoder
#define JPEG_QUALITY 85
#define AE_TARGET 115  // mean luma auto exposure aims for (full range)

struct stream {
    const char *isp_name;
    struct source *src;
    int width, height, kbps, gop;
    int isp, enc;
    int dmabuf[ISP_BUFS];
    const uint8_t *map[ISP_BUFS];  // the ISP buffers (uncached): auto exposure, raw frames
    size_t map_len, bpl;
    void *bs[ENC_BUFS];
    size_t bs_len[ENC_BUFS];
    pthread_t thread;
};

// Sensor exposure (lines) and analog gain (codes), stepped by ae_step;
// luma: the last mean luma measured
struct ae {
    int fd, expo, expo_min, expo_max, gain, gain_min, gain_max;
    unsigned luma;
};

static const struct config *cfg;
static volatile int running;
static struct stream hi_st, lo_st;
static int sensor = -1;  // the sensor's subdevice: exposure, gain, flips
static struct ae ae;
static struct isp_params params = {50, 50, 50, 50, 50, 0, 0};
static int night;

// Requests from other threads, served by the stream threads between frames
static pthread_mutex_t req_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t req_cond = PTHREAD_COND_INITIALIZER;
static int jpeg_fd = -1;
static void *jpeg_map;
static size_t jpeg_map_len;
static int jpeg_want;
static uint8_t *jpeg;
static size_t jpeg_len;
static int64_t jpeg_ms;
static uint8_t *raw_buf;
static size_t raw_cap;
static int raw_want, raw_chroma, raw_len, raw_w, raw_h;

// Serialises start, stop and restarts (a flip needs one)
static pthread_mutex_t life_lock = PTHREAD_MUTEX_INITIALIZER;

static int xioctl(int fd, unsigned long req, void *arg) {
    int r;
    do r = ioctl(fd, req, arg);
    while (r < 0 && errno == EINTR);
    return r;
}

// The device node (/dev/videoN or /dev/v4l-subdevN) whose name starts with prefix
static int open_named(const char *prefix) {
    DIR *d = opendir("/sys/class/video4linux");
    struct dirent *e;
    int fd = -1;
    if (!d)
        return -1;
    while (fd < 0 && (e = readdir(d))) {
        char path[300], name[64] = {0};
        snprintf(path, sizeof(path), "/sys/class/video4linux/%s/name", e->d_name);
        FILE *f = fopen(path, "r");
        if (!f)
            continue;
        if (fgets(name, sizeof(name), f) && !strncmp(name, prefix, strlen(prefix))) {
            snprintf(path, sizeof(path), "/dev/%s", e->d_name);
            fd = open(path, O_RDWR | O_CLOEXEC);
        }
        fclose(f);
    }
    closedir(d);
    return fd;
}

static int ctrl_set(int fd, unsigned id, int v) {
    struct v4l2_control c = {.id = id, .value = v};
    return xioctl(fd, VIDIOC_S_CTRL, &c);
}

static int ctrl_get(int fd, unsigned id, int *v) {
    struct v4l2_control c = {.id = id};
    if (xioctl(fd, VIDIOC_G_CTRL, &c) < 0)
        return -1;
    *v = c.value;
    return 0;
}

static int ctrl_range(int fd, unsigned id, int *min, int *max) {
    struct v4l2_queryctrl q = {.id = id};
    if (xioctl(fd, VIDIOC_QUERYCTRL, &q) < 0)
        return -1;
    *min = q.minimum;
    *max = q.maximum;
    return 0;
}

// A control by name (the ISP's noise reductions have no standard id)
static unsigned ctrl_by_name(int fd, const char *name) {
    struct v4l2_queryctrl q = {.id = V4L2_CTRL_FLAG_NEXT_CTRL};
    while (xioctl(fd, VIDIOC_QUERYCTRL, &q) == 0) {
        if (!strcmp((char *)q.name, name))
            return q.id;
        q.id |= V4L2_CTRL_FLAG_NEXT_CTRL;
    }
    return 0;
}

#define TRY(call)                                                  \
    do {                                                           \
        if ((call) < 0) {                                          \
            log_error("capture: %s: %s", #call, strerror(errno)); \
            return -1;                                             \
        }                                                          \
    } while (0)

// Auto exposure

// Mean of every 8th luma sample of every 8th line
static unsigned mean_luma(const uint8_t *y, int w, int h, size_t stride) {
    unsigned long sum = 0, n = 0;
    for (int r = 0; r < h; r += 8)
        for (int c = 0; c < w; c += 8, n++)
            sum += y[r * stride + c];
    return n ? sum / n : 0;
}

// Proportional step on exposure first, analog gain once the exposure is at
// its maximum, back down the same way (as capture/v4l2grab.c). A gain code
// is ~0.07 EV (jxf2x_again_log2); half the error a step, against overshoot.
static void ae_step(unsigned luma) {
    if (ae.fd < 0 || !luma)
        return;
    ae.luma = luma;
    double ratio = (double)AE_TARGET / luma;
    if (ratio > 0.9 && ratio < 1.1)
        return;
    int codes = (int)(fabs(log2(ratio)) / 0.07 / 2);
    codes = codes < 1 ? 1 : codes > 10 ? 10 : codes;
    if (ratio > 1 && ae.expo >= ae.expo_max && ae.gain < ae.gain_max)
        ae.gain = ae.gain + codes > ae.gain_max ? ae.gain_max : ae.gain + codes;
    else if (ratio < 1 && ae.gain > ae.gain_min)
        ae.gain = ae.gain - codes < ae.gain_min ? ae.gain_min : ae.gain - codes;
    else {
        int e = ae.expo * (ratio > 2 ? 2 : ratio < 0.5 ? 0.5 : ratio);
        ae.expo = e < ae.expo_min ? ae.expo_min : e > ae.expo_max ? ae.expo_max : e;
    }
    ctrl_set(ae.fd, V4L2_CID_EXPOSURE, ae.expo);
    ctrl_set(ae.fd, V4L2_CID_ANALOGUE_GAIN, ae.gain);
}

// A gain code's EV: jxf2x's codes step 1/16 within each octave
static double gain_ev(int code) {
    return code / 16 + log2(1 + (code % 16) / 16.0);
}

static void ae_init(int fd) {
    ae.fd = -1;
    ae.luma = 0;
    if (fd < 0 || ctrl_get(fd, V4L2_CID_EXPOSURE, &ae.expo) < 0 ||
        ctrl_get(fd, V4L2_CID_ANALOGUE_GAIN, &ae.gain) < 0 ||
        ctrl_range(fd, V4L2_CID_EXPOSURE, &ae.expo_min, &ae.expo_max) < 0 ||
        ctrl_range(fd, V4L2_CID_ANALOGUE_GAIN, &ae.gain_min, &ae.gain_max) < 0) {
        log_warn("capture: no exposure controls, auto exposure off");
        return;
    }
    ae.fd = fd;
}

// ISP settings

// Protect's 0-100 (50 neutral) onto a control's range around its default
static int level(int fd, unsigned id, int v) {
    struct v4l2_queryctrl q = {.id = id};
    if (!id || xioctl(fd, VIDIOC_QUERYCTRL, &q) < 0)
        return -1;
    v = v < 0 ? 0 : v > 100 ? 100 : v;
    int x = v <= 50 ? q.minimum + (q.default_value - q.minimum) * v / 50
                    : q.default_value + (q.maximum - q.default_value) * (v - 50) / 50;
    return ctrl_set(fd, id, x);
}

static void apply_isp(void) {
    int fd = hi_st.isp;
    level(fd, V4L2_CID_BRIGHTNESS, params.brightness);
    level(fd, V4L2_CID_CONTRAST, params.contrast);
    // Black and white at night (the IR cut filter is out)
    level(fd, V4L2_CID_SATURATION, night ? 0 : params.saturation);
    level(fd, V4L2_CID_SHARPNESS, params.sharpness);
    level(fd, ctrl_by_name(fd, "Noise Reduction"), params.denoise);
    level(fd, ctrl_by_name(fd, "Temporal Noise Reduction"), params.denoise);
}

// Streams

static int encoder_open(struct stream *s) {
    if ((s->enc = open_named("t20-h264")) < 0) {
        log_error("capture: no H.264 encoder (t20-vpu not loaded?)");
        return -1;
    }
    struct v4l2_format f = {.type = V4L2_BUF_TYPE_VIDEO_OUTPUT};
    f.fmt.pix.width = s->width;
    f.fmt.pix.height = s->height;
    f.fmt.pix.pixelformat = V4L2_PIX_FMT_NV12;
    f.fmt.pix.quantization = V4L2_QUANTIZATION_FULL_RANGE;
    TRY(xioctl(s->enc, VIDIOC_S_FMT, &f));
    struct v4l2_streamparm parm = {.type = V4L2_BUF_TYPE_VIDEO_OUTPUT};
    parm.parm.output.timeperframe = (struct v4l2_fract){1, cfg->fps};
    TRY(xioctl(s->enc, VIDIOC_S_PARM, &parm));
    TRY(ctrl_set(s->enc, V4L2_CID_MPEG_VIDEO_FRAME_RC_ENABLE, 1));
    TRY(ctrl_set(s->enc, V4L2_CID_MPEG_VIDEO_BITRATE, s->kbps * 1000));
    TRY(ctrl_set(s->enc, V4L2_CID_MPEG_VIDEO_GOP_SIZE, s->gop));
    ctrl_set(s->enc, V4L2_CID_MPEG_VIDEO_H264_MIN_QP, 15);
    ctrl_set(s->enc, V4L2_CID_MPEG_VIDEO_H264_MAX_QP, 40);

    struct v4l2_requestbuffers req = {
        .count = ISP_BUFS, .type = V4L2_BUF_TYPE_VIDEO_OUTPUT, .memory = V4L2_MEMORY_DMABUF};
    TRY(xioctl(s->enc, VIDIOC_REQBUFS, &req));
    req = (struct v4l2_requestbuffers){
        .count = ENC_BUFS, .type = V4L2_BUF_TYPE_VIDEO_CAPTURE, .memory = V4L2_MEMORY_MMAP};
    TRY(xioctl(s->enc, VIDIOC_REQBUFS, &req));
    for (unsigned i = 0; i < ENC_BUFS; i++) {
        struct v4l2_buffer b = {.type = req.type, .memory = req.memory, .index = i};
        TRY(xioctl(s->enc, VIDIOC_QUERYBUF, &b));
        s->bs[i] = mmap(NULL, b.length, PROT_READ, MAP_SHARED, s->enc, b.m.offset);
        if (s->bs[i] == MAP_FAILED) {
            s->bs[i] = NULL;
            TRY(-1);
        }
        s->bs_len[i] = b.length;
        TRY(xioctl(s->enc, VIDIOC_QBUF, &b));
    }
    enum v4l2_buf_type t = V4L2_BUF_TYPE_VIDEO_OUTPUT;
    TRY(xioctl(s->enc, VIDIOC_STREAMON, &t));
    t = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    TRY(xioctl(s->enc, VIDIOC_STREAMON, &t));
    return 0;
}

static int isp_open(struct stream *s) {
    if ((s->isp = open_named(s->isp_name)) < 0) {
        log_error("capture: no %s (t20-isp not loaded?)", s->isp_name);
        return -1;
    }
    struct v4l2_format f = {.type = V4L2_BUF_TYPE_VIDEO_CAPTURE};
    f.fmt.pix.width = s->width;
    f.fmt.pix.height = s->height;
    f.fmt.pix.pixelformat = V4L2_PIX_FMT_NV12;
    TRY(xioctl(s->isp, VIDIOC_S_FMT, &f));
    if ((int)f.fmt.pix.width != s->width || (int)f.fmt.pix.height != s->height) {
        log_error("capture: %s gives %ux%u, not %dx%d", s->isp_name, f.fmt.pix.width,
                  f.fmt.pix.height, s->width, s->height);
        return -1;
    }
    s->bpl = f.fmt.pix.bytesperline;
    // The low-resolution frames are read (auto exposure, raw frames): cached
    // buffers if the driver gives them (uncached reads run at a few MB/s)
    struct v4l2_requestbuffers req = {
        .count = ISP_BUFS, .type = V4L2_BUF_TYPE_VIDEO_CAPTURE, .memory = V4L2_MEMORY_MMAP,
        .flags = s == &lo_st ? V4L2_MEMORY_FLAG_NON_COHERENT : 0};
    TRY(xioctl(s->isp, VIDIOC_REQBUFS, &req));
    if (req.count != ISP_BUFS) {
        log_error("capture: %s: %u buffers", s->isp_name, req.count);
        return -1;
    }
    for (unsigned i = 0; i < ISP_BUFS; i++) {
        struct v4l2_buffer b = {.type = req.type, .memory = req.memory, .index = i};
        TRY(xioctl(s->isp, VIDIOC_QUERYBUF, &b));
        void *m = mmap(NULL, b.length, PROT_READ, MAP_SHARED, s->isp, b.m.offset);
        if (m == MAP_FAILED)
            TRY(-1);
        s->map[i] = m;
        s->map_len = b.length;
        struct v4l2_exportbuffer e = {.type = req.type, .index = i, .flags = O_RDONLY | O_CLOEXEC};
        TRY(xioctl(s->isp, VIDIOC_EXPBUF, &e));
        s->dmabuf[i] = e.fd;
        TRY(xioctl(s->isp, VIDIOC_QBUF, &b));
    }
    return 0;
}

static void stream_close(struct stream *s) {
    enum v4l2_buf_type t;
    if (s->isp >= 0) {
        t = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        xioctl(s->isp, VIDIOC_STREAMOFF, &t);
    }
    if (s->enc >= 0) {
        for (unsigned i = 0; i < ENC_BUFS; i++)
            if (s->bs[i])
                munmap(s->bs[i], s->bs_len[i]);
        close(s->enc);
    }
    for (unsigned i = 0; i < ISP_BUFS; i++) {
        if (s->dmabuf[i] >= 0)
            close(s->dmabuf[i]);
        if (s->map[i])
            munmap((void *)s->map[i], s->map_len);
    }
    if (s->isp >= 0)
        close(s->isp);
    s->isp = s->enc = -1;
    memset(s->map, 0, sizeof(s->map));
    memset(s->bs, 0, sizeof(s->bs));
    for (unsigned i = 0; i < ISP_BUFS; i++)
        s->dmabuf[i] = -1;
}

// The JPEG encoder: the main ISP buffers as dma-bufs in, one JFIF out
static int jpeg_open(void) {
    if ((jpeg_fd = open_named("t20-jpeg")) < 0) {
        log_warn("capture: no JPEG encoder, no snapshots");
        return -1;
    }
    ctrl_set(jpeg_fd, V4L2_CID_JPEG_COMPRESSION_QUALITY, JPEG_QUALITY);
    struct v4l2_format f = {.type = V4L2_BUF_TYPE_VIDEO_OUTPUT};
    f.fmt.pix.width = hi_st.width;
    f.fmt.pix.height = hi_st.height;
    f.fmt.pix.pixelformat = V4L2_PIX_FMT_NV12;
    TRY(xioctl(jpeg_fd, VIDIOC_S_FMT, &f));
    struct v4l2_requestbuffers req = {
        .count = ISP_BUFS, .type = V4L2_BUF_TYPE_VIDEO_OUTPUT, .memory = V4L2_MEMORY_DMABUF};
    TRY(xioctl(jpeg_fd, VIDIOC_REQBUFS, &req));
    req = (struct v4l2_requestbuffers){
        .count = 1, .type = V4L2_BUF_TYPE_VIDEO_CAPTURE, .memory = V4L2_MEMORY_MMAP};
    TRY(xioctl(jpeg_fd, VIDIOC_REQBUFS, &req));
    struct v4l2_buffer b = {.type = req.type, .memory = req.memory, .index = 0};
    TRY(xioctl(jpeg_fd, VIDIOC_QUERYBUF, &b));
    jpeg_map = mmap(NULL, b.length, PROT_READ, MAP_SHARED, jpeg_fd, b.m.offset);
    if (jpeg_map == MAP_FAILED) {
        jpeg_map = NULL;
        TRY(-1);
    }
    jpeg_map_len = b.length;
    TRY(xioctl(jpeg_fd, VIDIOC_QBUF, &b));
    enum v4l2_buf_type t = V4L2_BUF_TYPE_VIDEO_OUTPUT;
    TRY(xioctl(jpeg_fd, VIDIOC_STREAMON, &t));
    t = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    TRY(xioctl(jpeg_fd, VIDIOC_STREAMON, &t));
    return 0;
}

static void jpeg_close(void) {
    if (jpeg_map)
        munmap(jpeg_map, jpeg_map_len);
    if (jpeg_fd >= 0)
        close(jpeg_fd);
    jpeg_map = NULL;
    jpeg_fd = -1;
}

// One JPEG of ISP buffer i into jpeg; req_lock held
static void jpeg_encode(unsigned i, const struct v4l2_buffer *ib) {
    struct v4l2_buffer ob = {.type = V4L2_BUF_TYPE_VIDEO_OUTPUT, .memory = V4L2_MEMORY_DMABUF,
                             .index = i, .bytesused = hi_st.bpl * hi_st.height * 3 / 2};
    struct v4l2_buffer cb = {.type = V4L2_BUF_TYPE_VIDEO_CAPTURE, .memory = V4L2_MEMORY_MMAP};
    ob.m.fd = hi_st.dmabuf[i];
    ob.timestamp = ib->timestamp;
    if (xioctl(jpeg_fd, VIDIOC_QBUF, &ob) < 0 || xioctl(jpeg_fd, VIDIOC_DQBUF, &cb) < 0) {
        log_warn("capture: JPEG: %s", strerror(errno));
        return;
    }
    xioctl(jpeg_fd, VIDIOC_DQBUF, &ob);
    if (!(cb.flags & V4L2_BUF_FLAG_ERROR)) {
        uint8_t *p = realloc(jpeg, cb.bytesused);
        if (p) {
            memcpy(p, jpeg_map, cb.bytesused);
            jpeg = p;
            jpeg_len = cb.bytesused;
            jpeg_ms = mono_ms();
        }
    }
    cb.flags = 0;
    xioctl(jpeg_fd, VIDIOC_QBUF, &cb);
}

// Serve the requests that need this ISP frame (before it goes back)
static void serve(struct stream *s, unsigned i, const struct v4l2_buffer *ib, unsigned seq) {
    if (s == &lo_st && seq % 2 == 0)
        ae_step(mean_luma(s->map[i], s->width, s->height, s->bpl));
    if (!jpeg_want && !raw_want)
        return;
    pthread_mutex_lock(&req_lock);
    if (s == &hi_st && jpeg_want && jpeg_fd >= 0) {
        jpeg_encode(i, ib);
        jpeg_want = 0;
        pthread_cond_broadcast(&req_cond);
    }
    if (s == &lo_st && raw_want) {
        size_t y = (size_t)s->width * s->height, len = raw_chroma ? y * 3 / 2 : y;
        raw_len = -1;
        if (len <= raw_cap) {
            const uint8_t *p = s->map[i];
            for (int r = 0; r < s->height; r++)
                memcpy(raw_buf + (size_t)r * s->width, p + r * s->bpl, s->width);
            if (raw_chroma)
                for (int r = 0; r < s->height / 2; r++)
                    memcpy(raw_buf + y + (size_t)r * s->width,
                           p + s->bpl * s->height + r * s->bpl, s->width);
            raw_len = len;
            raw_w = s->width;
            raw_h = s->height;
        }
        raw_want = 0;
        pthread_cond_broadcast(&req_cond);
    }
    pthread_mutex_unlock(&req_lock);
}

// ISP frame -> encoder -> source, one frame at a time
static void *stream_main(void *arg) {
    struct stream *s = arg;
    unsigned seq = 0;
    while (running) {
        struct pollfd pfd = {.fd = s->isp, .events = POLLIN};
        if (poll(&pfd, 1, 1000) <= 0) {
            log_warn("capture: no frame from %s", s->isp_name);
            continue;
        }
        struct v4l2_buffer ib = {.type = V4L2_BUF_TYPE_VIDEO_CAPTURE, .memory = V4L2_MEMORY_MMAP};
        if (xioctl(s->isp, VIDIOC_DQBUF, &ib) < 0)
            continue;
        serve(s, ib.index, &ib, seq++);

        struct v4l2_buffer ob = {.type = V4L2_BUF_TYPE_VIDEO_OUTPUT, .memory = V4L2_MEMORY_DMABUF,
                                 .index = ib.index, .bytesused = s->bpl * s->height * 3 / 2};
        struct v4l2_buffer cb = {.type = V4L2_BUF_TYPE_VIDEO_CAPTURE, .memory = V4L2_MEMORY_MMAP};
        ob.m.fd = s->dmabuf[ib.index];
        ob.timestamp = ib.timestamp;
        if (xioctl(s->enc, VIDIOC_QBUF, &ob) == 0 && xioctl(s->enc, VIDIOC_DQBUF, &cb) == 0) {
            xioctl(s->enc, VIDIOC_DQBUF, &ob);
            if (!(cb.flags & V4L2_BUF_FLAG_ERROR) && cb.bytesused) {
                struct frame *f = frame_new(cb.bytesused);
                if (f) {
                    memcpy(f->data, s->bs[cb.index], cb.bytesused);
                    f->ts_ms = (int64_t)cb.timestamp.tv_sec * 1000 + cb.timestamp.tv_usec / 1000;
                    source_push(s->src, f);
                    frame_unref(f);
                }
            }
            cb.flags = 0;
            xioctl(s->enc, VIDIOC_QBUF, &cb);
        } else {
            log_warn("capture: %s encoder: %s", s->isp_name, strerror(errno));
        }
        xioctl(s->isp, VIDIOC_QBUF, &ib);
    }
    return NULL;
}

// Start and stop

static int sensor_flips(void) {
    if (sensor < 0)
        return 0;
    TRY(ctrl_set(sensor, V4L2_CID_VFLIP, cfg->flip_v ^ !!params.flip));
    TRY(ctrl_set(sensor, V4L2_CID_HFLIP, cfg->flip_h ^ !!params.mirror));
    return 0;
}

static int start(void) {
    struct stream *ss[2] = {&hi_st, &lo_st};
    for (int i = 0; i < 2; i++) {
        ss[i]->isp = ss[i]->enc = -1;
        for (unsigned j = 0; j < ISP_BUFS; j++)
            ss[i]->dmabuf[j] = -1;
    }
    if (sensor < 0 && (sensor = open_named("jxf2x")) < 0)
        log_warn("capture: no sensor subdevice: no exposure control, no flips");
    if (sensor_flips() < 0)
        return -1;
    for (int i = 0; i < 2; i++)
        if (isp_open(ss[i]) < 0 || encoder_open(ss[i]) < 0)
            return -1;
    jpeg_open();
    ae_init(sensor);
    apply_isp();
    // Both ISP outputs before either starts the sensor's stream
    for (int i = 0; i < 2; i++) {
        enum v4l2_buf_type t = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        TRY(xioctl(ss[i]->isp, VIDIOC_STREAMON, &t));
    }
    running = 1;
    for (int i = 0; i < 2; i++)
        pthread_create(&ss[i]->thread, NULL, stream_main, ss[i]);
    return 0;
}

static void stop(void) {
    if (running) {
        running = 0;
        pthread_join(hi_st.thread, NULL);
        pthread_join(lo_st.thread, NULL);
    }
    pthread_mutex_lock(&req_lock);
    jpeg_want = raw_want = 0;
    pthread_cond_broadcast(&req_cond);
    pthread_mutex_unlock(&req_lock);
    jpeg_close();
    stream_close(&lo_st);
    stream_close(&hi_st);
}

int capture_v4l2_available(void) {
    int fd = open_named("t20-isp-main");
    if (fd < 0)
        return 0;
    close(fd);
    return 1;
}

int capture_v4l2_start(const struct config *c, struct source *hi, struct source *lo) {
    cfg = c;
    // The sensor's native size; the T20's sensors here are all 1080p.
    hi_st = (struct stream){.isp_name = "t20-isp-main", .src = hi, .width = 1920, .height = 1080,
                            .kbps = c->hi_kbps, .gop = c->hi_gop};
    lo_st = (struct stream){.isp_name = "t20-isp-sub", .src = lo, .width = c->lo_width,
                            .height = c->lo_height, .kbps = c->lo_kbps, .gop = c->lo_gop};
    pthread_mutex_lock(&life_lock);
    int rc = start();
    if (rc < 0)
        stop();
    pthread_mutex_unlock(&life_lock);
    if (rc == 0)
        log_info("capture: V4L2, 1920x1080 @ %d kbit/s, %dx%d @ %d kbit/s, %d fps", c->hi_kbps,
                 c->lo_width, c->lo_height, c->lo_kbps, c->fps);
    return rc;
}

void capture_v4l2_stop(void) {
    pthread_mutex_lock(&life_lock);
    stop();
    if (sensor >= 0)
        close(sensor);
    sensor = -1;
    pthread_mutex_unlock(&life_lock);
    log_info("capture: stopped");
}

// Wait for a stream thread to serve a request; req_lock held
static int wait_served(volatile int *want) {
    struct timespec t;
    clock_gettime(CLOCK_REALTIME, &t);
    t.tv_sec += 2;
    while (*want && running)
        if (pthread_cond_timedwait(&req_cond, &req_lock, &t) == ETIMEDOUT)
            break;
    int ok = !*want;
    *want = 0;
    return ok ? 0 : -1;
}

// The exposure the scene needs to reach the target luma, as the vendor ISP's
// "exposure log2 id" (log2(lines x gain) x 65536, more than the sensor can
// give in the dark): the applied exposure plus what the frames still miss
int capture_v4l2_exposure(void) {
    if (!running || ae.fd < 0 || !ae.luma)
        return -1;
    double ev = log2(ae.expo) + gain_ev(ae.gain) + log2((double)AE_TARGET / ae.luma);
    return ev < 0 ? 0 : (int)(ev * 65536);
}

int capture_v4l2_jpeg(uint8_t **data, size_t *len) {
    int rc = -1;
    pthread_mutex_lock(&req_lock);
    if (running && jpeg_fd >= 0 && (!jpeg || mono_ms() - jpeg_ms > 500)) {
        jpeg_want = 1;
        wait_served(&jpeg_want);
    }
    if (running && jpeg && mono_ms() - jpeg_ms <= 2500 && (*data = malloc(jpeg_len))) {
        memcpy(*data, jpeg, jpeg_len);
        *len = jpeg_len;
        rc = 0;
    }
    pthread_mutex_unlock(&req_lock);
    return rc;
}

int capture_v4l2_raw(uint8_t *buf, size_t cap, int *w, int *h, int chroma) {
    int n = -1;
    pthread_mutex_lock(&req_lock);
    if (running) {
        raw_buf = buf;
        raw_cap = cap;
        raw_chroma = chroma;
        raw_want = 1;
        if (wait_served(&raw_want) == 0 && raw_len > 0) {
            n = raw_len;
            *w = raw_w;
            *h = raw_h;
        }
        raw_buf = NULL;
    }
    pthread_mutex_unlock(&req_lock);
    return n;
}

void capture_v4l2_isp(const struct isp_params *p) {
    pthread_mutex_lock(&life_lock);
    int flips = p->flip != params.flip || p->mirror != params.mirror;
    params = *p;
    if (running) {
        // A flip changes the sensor's Bayer order: the ISP restarts with it
        if (flips) {
            log_info("capture: restarting for %s%s", p->flip ? "flip " : "",
                     p->mirror ? "mirror" : "");
            stop();
            if (start() < 0) {
                log_error("capture: restart failed");
                stop();
            }
        } else {
            apply_isp();
        }
    }
    pthread_mutex_unlock(&life_lock);
    log_info("capture: image brightness %d contrast %d saturation %d sharpness %d denoise %d%s%s",
             p->brightness, p->contrast, p->saturation, p->sharpness, p->denoise,
             p->flip ? " flip" : "", p->mirror ? " mirror" : "");
}

void capture_v4l2_night(int on) {
    pthread_mutex_lock(&life_lock);
    night = on;
    if (running)
        apply_isp();
    pthread_mutex_unlock(&life_lock);
    log_info("capture: %s mode", on ? "night" : "day");
}

#else  // no V4L2 on this host

int capture_v4l2_available(void) { return 0; }
int capture_v4l2_start(const struct config *c, struct source *hi, struct source *lo) { return -1; }
void capture_v4l2_stop(void) {}
int capture_v4l2_jpeg(uint8_t **data, size_t *len) { return -1; }
int capture_v4l2_raw(uint8_t *buf, size_t cap, int *w, int *h, int chroma) { return -1; }
int capture_v4l2_exposure(void) { return -1; }
void capture_v4l2_isp(const struct isp_params *p) {}
void capture_v4l2_night(int on) {}

#endif
