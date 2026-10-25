#include "capture.h"

#include "util.h"

#ifdef UVC_IMP

#include <errno.h>
#include <fcntl.h>
#include <linux/videodev2.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include "imp_common.h"
#include "imp_encoder.h"
#include "imp_framesource.h"
#include "imp_isp.h"
#include "imp_system.h"

// Frame source channel = encoder group; channel numbers are libimp's.
enum { FS_HI = 0, FS_LO = 1 };
enum { ENC_HI = 0, ENC_LO = 1, ENC_JPEG = 2 };

struct encoder {
    int chn;
    struct source *src;
    const char *mirror;
    int width, height;
    pthread_t thread;
};

static const struct config *cfg;
static IMPSensorInfo sensor;
static volatile int running;
static struct encoder encoders[2];
static pthread_t night_thread, jpeg_thread;

static pthread_mutex_t jpeg_lock = PTHREAD_MUTEX_INITIALIZER;
static uint8_t *jpeg;
static size_t jpeg_len;
static int64_t jpeg_ms;

static pthread_mutex_t raw_lock = PTHREAD_MUTEX_INITIALIZER;

#define TRY(call)                                                \
    do {                                                         \
        if ((call) < 0) {                                        \
            log_error("capture: %s failed", #call);              \
            return -1;                                           \
        }                                                        \
    } while (0)

// The sensor driver S60camera loaded reports itself as "sensor :jxf23".
static int sensor_info(IMPSensorInfo *s) {
    char line[64] = {0}, *name;
    FILE *fp = fopen("/proc/jz/sinfo/info", "r");
    int ok = fp && fgets(line, sizeof(line), fp) && (name = strchr(line, ':'));
    if (fp)
        fclose(fp);
    if (!ok) {
        log_error("capture: no sensor in /proc/jz/sinfo/info (camera modules not loaded?)");
        return -1;
    }
    name++;
    name[strcspn(name, " \n")] = 0;
    memset(s, 0, sizeof(*s));
    snprintf(s->name, sizeof(s->name), "%s", name);
    s->cbus_type = TX_SENSOR_CONTROL_INTERFACE_I2C;
    snprintf(s->i2c.type, sizeof(s->i2c.type), "%s", name);
    s->i2c.addr = 0x40;
    log_info("capture: sensor %s", name);
    return 0;
}

static int frame_source(int chn, int w, int h) {
    IMPFSChnAttr a;
    memset(&a, 0, sizeof(a));
    a.pixFmt = PIX_FMT_NV12;
    a.outFrmRateNum = cfg->fps;
    a.outFrmRateDen = 1;
    a.nrVBs = 2;
    a.type = FS_PHY_CHANNEL;
    a.scaler.enable = 1;
    a.scaler.outwidth = w;
    a.scaler.outheight = h;
    a.picWidth = w;
    a.picHeight = h;
    TRY(IMP_FrameSource_CreateChn(chn, &a));
    TRY(IMP_FrameSource_SetChnAttr(chn, &a));
    return 0;
}

static int encoder_chn(int group, int chn, int type, int w, int h, int kbps, int gop) {
    IMPEncoderCHNAttr a;
    memset(&a, 0, sizeof(a));
    a.encAttr.enType = type;
    a.encAttr.picWidth = w;
    a.encAttr.picHeight = h;
    if (type == PT_H264) {
        IMPEncoderAttrH264VBR *v = &a.rcAttr.attrH264Vbr;
        a.rcAttr.rcMode = ENC_RC_MODE_H264VBR;
        v->outFrmRate.frmRateNum = cfg->fps;
        v->outFrmRate.frmRateDen = 1;
        v->maxGop = gop;
        v->maxQp = 38;
        v->minQp = 15;
        v->staticTime = 1;
        v->maxBitRate = kbps;
        v->changePos = 50;
        v->FrmQPStep = 3;
        v->GOPQPStep = 15;
        a.rcAttr.attrH264FrmUsed.enable = 1;
    }
    TRY(IMP_Encoder_CreateChn(chn, &a));
    TRY(IMP_Encoder_RegisterChn(group, chn));
    return 0;
}

static int bind(int fs, int group, int on) {
    IMPCell src = {DEV_ID_FS, fs, 0}, dst = {DEV_ID_ENC, group, 0};
    return on ? IMP_System_Bind(&src, &dst) : IMP_System_UnBind(&src, &dst);
}

// v4l2loopback output, as OpenMiko's videocapture sets it up; each write()
// is one frame for the reader (v4l2rtspserver, mjpg_streamer, getimage).
static int open_mirror(const char *path, int w, int h, uint32_t fourcc) {
    int fd = open(path, O_WRONLY);
    if (fd < 0) {
        log_warn("capture: %s: %s", path, strerror(errno));
        return -1;
    }
    struct v4l2_format fmt;
    memset(&fmt, 0, sizeof(fmt));
    fmt.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
    fmt.fmt.pix.width = w;
    fmt.fmt.pix.height = h;
    fmt.fmt.pix.pixelformat = fourcc;
    fmt.fmt.pix.field = V4L2_FIELD_NONE;
    fmt.fmt.pix.colorspace = fourcc == V4L2_PIX_FMT_MJPEG ? V4L2_COLORSPACE_JPEG : 0;
    if (ioctl(fd, VIDIOC_S_FMT, &fmt) < 0 || ioctl(fd, VIDIOC_STREAMON, &fmt.type) < 0) {
        log_warn("capture: %s: cannot set up v4l2 output: %s", path, strerror(errno));
        close(fd);
        return -1;
    }
    return fd;
}

static size_t stream_len(const IMPEncoderStream *st) {
    size_t len = 0;
    for (uint32_t i = 0; i < st->packCount; i++)
        len += st->pack[i].length;
    return len;
}

// Packs of one frame may wrap around the end of the stream buffer, so they
// are gathered individually.
static void stream_copy(const IMPEncoderStream *st, uint8_t *dst) {
    for (uint32_t i = 0; i < st->packCount; i++) {
        memcpy(dst, (const void *)st->pack[i].virAddr, st->pack[i].length);
        dst += st->pack[i].length;
    }
}

static void *encoder_main(void *arg) {
    struct encoder *e = arg;
    int fd = *e->mirror ? open_mirror(e->mirror, e->width, e->height, V4L2_PIX_FMT_H264) : -1;
    while (running) {
        if (IMP_Encoder_PollingStream(e->chn, 1000) < 0) {
            log_warn("capture: no frame from encoder %d", e->chn);
            continue;
        }
        IMPEncoderStream st;
        if (IMP_Encoder_GetStream(e->chn, &st, 1) < 0)
            continue;
        struct frame *f = frame_new(stream_len(&st));
        if (f) {
            stream_copy(&st, f->data);
            // libimp's clock was rebased onto mono_ms() at start.
            f->ts_ms = st.pack[0].timestamp / 1000;
        }
        IMP_Encoder_ReleaseStream(e->chn, &st);
        if (!f)
            continue;
        if (fd >= 0 && write(fd, f->data, f->len) < 0) {
            log_warn("capture: %s: %s; no longer mirroring", e->mirror, strerror(errno));
            close(fd);
            fd = -1;
        }
        source_push(e->src, f);
        frame_unref(f);
    }
    if (fd >= 0)
        close(fd);
    return NULL;
}

// One frame from the JPEG channel, which shares the full-resolution group
// with the H.264 encoder and only encodes while receiving. jpeg_lock held.
static int jpeg_grab(void) {
    if (IMP_Encoder_StartRecvPic(ENC_JPEG) < 0)
        return -1;
    int rc = -1;
    IMPEncoderStream st;
    if (IMP_Encoder_PollingStream(ENC_JPEG, 1000) == 0 && IMP_Encoder_GetStream(ENC_JPEG, &st, 1) == 0) {
        size_t len = stream_len(&st);
        uint8_t *p = realloc(jpeg, len);
        if (p) {
            stream_copy(&st, p);
            jpeg = p;
            jpeg_len = len;
            jpeg_ms = mono_ms();
            rc = 0;
        }
        IMP_Encoder_ReleaseStream(ENC_JPEG, &st);
    }
    IMP_Encoder_StopRecvPic(ENC_JPEG);
    return rc;
}

int capture_jpeg(uint8_t **data, size_t *len) {
    int rc = 0;
    pthread_mutex_lock(&jpeg_lock);
    if (!running)
        rc = -1;
    else if (!jpeg || mono_ms() - jpeg_ms > 500)
        rc = jpeg_grab();
    if (rc == 0 && (*data = malloc(jpeg_len))) {
        memcpy(*data, jpeg, jpeg_len);
        *len = jpeg_len;
    } else {
        rc = -1;
    }
    pthread_mutex_unlock(&jpeg_lock);
    return rc;
}

// About one JPEG a second for readers of the old MJPEG device (getimage,
// mjpg_streamer, the RTSP server's second stream).
static void *jpeg_main(void *arg) {
    // MJPG, not JPEG (videocapture's choice): v4l2rtspserver only accepts MJPG.
    int fd = open_mirror(cfg->jpeg_mirror, encoders[0].width, encoders[0].height, V4L2_PIX_FMT_MJPEG);
    while (running && fd >= 0) {
        uint8_t *p;
        size_t len;
        if (capture_jpeg(&p, &len) == 0) {
            if (write(fd, p, len) < 0)
                log_debug("capture: %s: %s", cfg->jpeg_mirror, strerror(errno));
            free(p);
        }
        sleep_ms(1000);
    }
    if (fd >= 0)
        close(fd);
    return NULL;
}

int capture_raw(uint8_t *buf, size_t cap, int *w, int *h, int chroma) {
    IMPFrameInfo *f;
    pthread_mutex_lock(&raw_lock);
    int n = -1;
    if (running && IMP_FrameSource_GetFrame(FS_LO, &f) == 0) {
        size_t len = (size_t)f->width * f->height * (chroma ? 3 : 2) / 2;
        if (len <= cap) {
            memcpy(buf, (const void *)f->virAddr, len);
            *w = f->width;
            *h = f->height;
            n = len;
        }
        IMP_FrameSource_ReleaseFrame(FS_LO, f);
    }
    pthread_mutex_unlock(&raw_lock);
    return n;
}

// OpenMiko's autonight switches the IR cut filter and LEDs itself and
// writes its decision to a file for the capture process, which sets the
// matching ISP mode (as videocapture does).
static void *night_main(void *arg) {
    int prev = -1;
    while (running) {
        int night = 0;
        FILE *fp = fopen(cfg->night_file, "r");
        if (fp) {
            if (fscanf(fp, "%d", &night) != 1)
                night = 0;
            fclose(fp);
        }
        night = night != 0;
        if (night != prev) {
            log_info("capture: %s mode", night ? "night" : "day");
            IMP_ISP_Tuning_SetISPRunningMode(night ? IMPISP_RUNNING_MODE_NIGHT : IMPISP_RUNNING_MODE_DAY);
            IMP_ISP_Tuning_SetSceneMode(night ? IMPISP_SCENE_MODE_NIGHT : IMPISP_SCENE_MODE_AUTO);
            IMP_ISP_Tuning_SetColorfxMode(night ? IMPISP_COLORFX_MODE_BW : IMPISP_COLORFX_MODE_AUTO);
            prev = night;
        }
        sleep_ms(1000);
    }
    return NULL;
}

// Protect's 0-100 (50 neutral) to libimp's 0-255 (128 neutral).
static unsigned char isp_level(int v) {
    v = v < 0 ? 0 : v > 100 ? 100 : v;
    return v <= 50 ? v * 128 / 50 : 128 + (v - 50) * 127 / 50;
}

void capture_isp(const struct isp_params *p) {
    if (!running)
        return;
    IMP_ISP_Tuning_SetBrightness(isp_level(p->brightness));
    IMP_ISP_Tuning_SetContrast(isp_level(p->contrast));
    IMP_ISP_Tuning_SetSaturation(isp_level(p->saturation));
    IMP_ISP_Tuning_SetSharpness(isp_level(p->sharpness));
    // 3D noise reduction strength in percent of the tuning's, 50-150.
    IMP_ISP_Tuning_SetTemperStrength(50 + (p->denoise < 0 ? 0 : p->denoise > 100 ? 100 : p->denoise));
    IMP_ISP_Tuning_SetISPVflip(cfg->flip_v ^ !!p->flip ? IMPISP_TUNING_OPS_MODE_ENABLE
                                                       : IMPISP_TUNING_OPS_MODE_DISABLE);
    IMP_ISP_Tuning_SetISPHflip(cfg->flip_h ^ !!p->mirror ? IMPISP_TUNING_OPS_MODE_ENABLE
                                                         : IMPISP_TUNING_OPS_MODE_DISABLE);
    log_info("capture: image brightness %d contrast %d saturation %d sharpness %d denoise %d%s%s",
             p->brightness, p->contrast, p->saturation, p->sharpness, p->denoise,
             p->flip ? " flip" : "", p->mirror ? " mirror" : "");
}

static int isp_start(void) {
    if (sensor_info(&sensor) < 0)
        return -1;
    TRY(IMP_ISP_Open());
    TRY(IMP_ISP_AddSensor(&sensor));
    TRY(IMP_ISP_EnableSensor());
    TRY(IMP_System_Init());
    TRY(IMP_ISP_EnableTuning());
    // Encoder timestamps in the same clock as everything else.
    IMP_System_RebaseTimeStamp(mono_ms() * 1000);
    IMP_ISP_Tuning_SetWDRAttr(IMPISP_TUNING_OPS_MODE_DISABLE);
    IMP_ISP_Tuning_SetSharpness(50);
    IMP_ISP_Tuning_SetISPVflip(cfg->flip_v ? IMPISP_TUNING_OPS_MODE_ENABLE : IMPISP_TUNING_OPS_MODE_DISABLE);
    IMP_ISP_Tuning_SetISPHflip(cfg->flip_h ? IMPISP_TUNING_OPS_MODE_ENABLE : IMPISP_TUNING_OPS_MODE_DISABLE);
    return 0;
}

int capture_start(const struct config *c, struct source *hi, struct source *lo) {
    cfg = c;
    // The sensor's native size; the T20's sensors here are all 1080p.
    encoders[0] = (struct encoder){.chn = ENC_HI, .src = hi, .mirror = c->hi_mirror, .width = 1920, .height = 1080};
    encoders[1] = (struct encoder){
        .chn = ENC_LO, .src = lo, .mirror = c->lo_mirror, .width = c->lo_width, .height = c->lo_height};

    if (isp_start() < 0)
        return -1;
    if (frame_source(FS_HI, 1920, 1080) < 0 || frame_source(FS_LO, c->lo_width, c->lo_height) < 0)
        return -1;
    IMP_Encoder_CreateGroup(FS_HI);
    IMP_Encoder_CreateGroup(FS_LO);
    if (encoder_chn(FS_HI, ENC_HI, PT_H264, 1920, 1080, c->hi_kbps, c->hi_gop) < 0 ||
        encoder_chn(FS_LO, ENC_LO, PT_H264, c->lo_width, c->lo_height, c->lo_kbps, c->lo_gop) < 0 ||
        encoder_chn(FS_HI, ENC_JPEG, PT_JPEG, 1920, 1080, 0, 0) < 0)
        return -1;
    TRY(bind(FS_HI, FS_HI, 1));
    TRY(bind(FS_LO, FS_LO, 1));
    TRY(IMP_FrameSource_EnableChn(FS_HI));
    TRY(IMP_FrameSource_EnableChn(FS_LO));
    // Keep one frame of the bound low-res channel for capture_raw (only
    // possible once the channel is enabled).
    TRY(IMP_FrameSource_SetFrameDepth(FS_LO, 1));
    TRY(IMP_Encoder_StartRecvPic(ENC_HI));
    TRY(IMP_Encoder_StartRecvPic(ENC_LO));

    running = 1;
    for (int i = 0; i < 2; i++)
        pthread_create(&encoders[i].thread, NULL, encoder_main, &encoders[i]);
    pthread_create(&night_thread, NULL, night_main, NULL);
    if (*c->jpeg_mirror)
        pthread_create(&jpeg_thread, NULL, jpeg_main, NULL);
    log_info("capture: 1920x1080 @ %d kbit/s, %dx%d @ %d kbit/s, %d fps", c->hi_kbps, c->lo_width,
             c->lo_height, c->lo_kbps, c->fps);
    return 0;
}

// Tear down in the reverse order of capture_start, so the next owner of
// libimp (a restarted uvcd, or videocapture) finds the hardware idle.
void capture_stop(void) {
    if (!running)
        return;
    pthread_mutex_lock(&jpeg_lock);
    pthread_mutex_lock(&raw_lock);
    running = 0;
    pthread_mutex_unlock(&raw_lock);
    pthread_mutex_unlock(&jpeg_lock);
    for (int i = 0; i < 2; i++)
        pthread_join(encoders[i].thread, NULL);
    pthread_join(night_thread, NULL);
    if (*cfg->jpeg_mirror)
        pthread_join(jpeg_thread, NULL);
    IMP_Encoder_StopRecvPic(ENC_HI);
    IMP_Encoder_StopRecvPic(ENC_LO);
    bind(FS_HI, FS_HI, 0);
    bind(FS_LO, FS_LO, 0);
    for (int chn = ENC_HI; chn <= ENC_JPEG; chn++) {
        IMP_Encoder_UnRegisterChn(chn);
        IMP_Encoder_DestroyChn(chn);
    }
    IMP_Encoder_DestroyGroup(FS_HI);
    IMP_Encoder_DestroyGroup(FS_LO);
    IMP_FrameSource_DisableChn(FS_HI);
    IMP_FrameSource_DisableChn(FS_LO);
    IMP_FrameSource_DestroyChn(FS_HI);
    IMP_FrameSource_DestroyChn(FS_LO);
    IMP_System_Exit();
    IMP_ISP_DisableSensor();
    IMP_ISP_DelSensor(&sensor);
    IMP_ISP_DisableTuning();
    IMP_ISP_Close();
    log_info("capture: stopped");
}

#else // host builds: no camera

int capture_start(const struct config *c, struct source *hi, struct source *lo) {
    log_error("capture: this build has no libimp support (CAPTURE=1 needs the camera build)");
    return -1;
}

void capture_stop(void) {}

int capture_jpeg(uint8_t **data, size_t *len) { return -1; }

int capture_raw(uint8_t *buf, size_t cap, int *w, int *h, int chroma) { return -1; }

void capture_isp(const struct isp_params *p) {}

#endif
