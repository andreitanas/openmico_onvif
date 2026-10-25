// Feasibility test for the full capture pipeline under musl: two frame
// sources bound to two H.264 encoders (1080p + 640x360) while raw frames
// are pulled from the 640x360 source at the same time.
// Run with OpenMiko's videocapture stopped.
//   enctest seconds        writes /tmp/hi.h264, /tmp/lo.h264, /tmp/raw.nv12

#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "imp_common.h"
#include "imp_encoder.h"
#include "imp_framesource.h"
#include "imp_isp.h"
#include "imp_system.h"

#define CHECK(call)                                                   \
    do {                                                              \
        int rc_ = (call);                                             \
        printf("%-62s %s\n", #call, rc_ < 0 ? "FAILED" : "ok");      \
        if (rc_ < 0)                                                  \
            exit(1);                                                  \
    } while (0)

static volatile int running = 1;

static double now(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

static void on_crash(int sig, siginfo_t *si, void *ctx) {
    const unsigned char *mc = (const unsigned char *)&((ucontext_t *)ctx)->uc_mcontext;
    uint64_t pc, ra;
    memcpy(&pc, mc + 8, 8);
    memcpy(&ra, mc + 16 + 31 * 8, 8);
    fprintf(stderr, "signal %d at pc=0x%08x ra=0x%08x addr=%p\n", sig, (unsigned)pc, (unsigned)ra,
            si->si_addr);
    _exit(128 + sig);
}

static void framesource(int chn, int w, int h) {
    IMPFSChnAttr a;
    memset(&a, 0, sizeof(a));
    a.pixFmt = PIX_FMT_NV12;
    a.outFrmRateNum = 25;
    a.outFrmRateDen = 1;
    a.nrVBs = 2;
    a.type = FS_PHY_CHANNEL;
    a.scaler.enable = 1;
    a.scaler.outwidth = w;
    a.scaler.outheight = h;
    a.picWidth = w;
    a.picHeight = h;
    CHECK(IMP_FrameSource_CreateChn(chn, &a));
    CHECK(IMP_FrameSource_SetChnAttr(chn, &a));
}

static void encoder(int group, int chn, int w, int h, int kbps, int gop) {
    IMPEncoderCHNAttr a;
    memset(&a, 0, sizeof(a));
    a.encAttr.enType = PT_H264;
    a.encAttr.profile = 0;
    a.encAttr.picWidth = w;
    a.encAttr.picHeight = h;
    a.rcAttr.rcMode = ENC_RC_MODE_H264VBR;
    a.rcAttr.attrH264Vbr.outFrmRate.frmRateNum = 25;
    a.rcAttr.attrH264Vbr.outFrmRate.frmRateDen = 1;
    a.rcAttr.attrH264Vbr.maxGop = gop;
    a.rcAttr.attrH264Vbr.maxQp = 38;
    a.rcAttr.attrH264Vbr.minQp = 15;
    a.rcAttr.attrH264Vbr.staticTime = 1;
    a.rcAttr.attrH264Vbr.maxBitRate = kbps;
    a.rcAttr.attrH264Vbr.changePos = 50;
    a.rcAttr.attrH264Vbr.FrmQPStep = 3;
    a.rcAttr.attrH264Vbr.GOPQPStep = 15;
    a.rcAttr.attrH264FrmUsed.enable = 1;
    IMP_Encoder_CreateGroup(group); // fails harmlessly if it exists
    CHECK(IMP_Encoder_CreateChn(chn, &a));
    CHECK(IMP_Encoder_RegisterChn(group, chn));
}

static void bind_fs_to_enc(int fs, int group) {
    IMPCell src = {DEV_ID_FS, fs, 0}, dst = {DEV_ID_ENC, group, 0};
    CHECK(IMP_System_Bind(&src, &dst));
}

struct enc_stats {
    int chn;
    const char *path;
    int frames, keys;
    long bytes;
};

static void *encoder_loop(void *arg) {
    struct enc_stats *s = arg;
    FILE *fp = fopen(s->path, "wb");
    while (running) {
        if (IMP_Encoder_PollingStream(s->chn, 1000) < 0)
            continue;
        IMPEncoderStream st;
        if (IMP_Encoder_GetStream(s->chn, &st, 1) < 0)
            continue;
        for (uint32_t i = 0; i < st.packCount; i++) {
            fwrite((void *)st.pack[i].virAddr, 1, st.pack[i].length, fp);
            s->bytes += st.pack[i].length;
            if (st.pack[i].dataType.h264Type == IMP_NAL_SLICE_IDR)
                s->keys++;
        }
        s->frames++;
        IMP_Encoder_ReleaseStream(s->chn, &st);
    }
    fclose(fp);
    return NULL;
}

int main(int argc, char **argv) {
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = on_crash;
    sa.sa_flags = SA_SIGINFO;
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGBUS, &sa, NULL);
    setvbuf(stdout, NULL, _IONBF, 0);
    int seconds = argc > 1 ? atoi(argv[1]) : 6;

    char line[64] = {0}, *name;
    FILE *fp = fopen("/proc/jz/sinfo/info", "r");
    if (!fp || !fgets(line, sizeof(line), fp) || !(name = strchr(line, ':')))
        return 1;
    fclose(fp);
    name++;
    name[strcspn(name, "\n")] = 0;
    IMPSensorInfo sensor;
    memset(&sensor, 0, sizeof(sensor));
    snprintf(sensor.name, sizeof(sensor.name), "%s", name);
    sensor.cbus_type = TX_SENSOR_CONTROL_INTERFACE_I2C;
    snprintf(sensor.i2c.type, sizeof(sensor.i2c.type), "%s", name);
    sensor.i2c.addr = 0x40;

    CHECK(IMP_ISP_Open());
    CHECK(IMP_ISP_AddSensor(&sensor));
    CHECK(IMP_ISP_EnableSensor());
    CHECK(IMP_System_Init());
    CHECK(IMP_ISP_EnableTuning());

    framesource(0, 1920, 1080);
    framesource(1, 640, 360);
    encoder(0, 0, 1920, 1080, 2000, 50);
    encoder(1, 1, 640, 360, 300, 25);
    bind_fs_to_enc(0, 0);
    bind_fs_to_enc(1, 1);
    CHECK(IMP_FrameSource_EnableChn(0));
    CHECK(IMP_FrameSource_EnableChn(1));
    CHECK(IMP_FrameSource_SetFrameDepth(1, 1)); // raw copies of the bound 640x360 source
    CHECK(IMP_Encoder_StartRecvPic(0));
    CHECK(IMP_Encoder_StartRecvPic(1));

    struct enc_stats hi = {0, "/tmp/hi.h264"}, lo = {1, "/tmp/lo.h264"};
    pthread_t th, tl;
    pthread_create(&th, NULL, encoder_loop, &hi);
    pthread_create(&tl, NULL, encoder_loop, &lo);

    // Raw frames for motion/detection: ~5 per second.
    int raw = 0;
    double t0 = now(), grab = 0;
    while (now() - t0 < seconds) {
        IMPFrameInfo *f;
        double g0 = now();
        if (IMP_FrameSource_GetFrame(1, &f) == 0) {
            grab += now() - g0;
            if (++raw == 10) {
                FILE *o = fopen("/tmp/raw.nv12", "wb");
                fwrite((void *)f->virAddr, 1, f->width * f->height * 3 / 2, o);
                fclose(o);
            }
            IMP_FrameSource_ReleaseFrame(1, f);
        }
        usleep(200000);
    }
    running = 0;
    pthread_join(th, NULL);
    pthread_join(tl, NULL);
    double dt = now() - t0;
    printf("hi: %d frames (%.1f fps), %d IDR, %.0f kbit/s\n", hi.frames, hi.frames / dt, hi.keys,
           hi.bytes * 8 / dt / 1000);
    printf("lo: %d frames (%.1f fps), %d IDR, %.0f kbit/s\n", lo.frames, lo.frames / dt, lo.keys,
           lo.bytes * 8 / dt / 1000);
    printf("raw: %d frames grabbed alongside, %.1f ms per GetFrame\n", raw, raw ? grab / raw * 1000 : 0);

    IMP_Encoder_StopRecvPic(0);
    IMP_Encoder_StopRecvPic(1);
    IMPCell s0 = {DEV_ID_FS, 0, 0}, d0 = {DEV_ID_ENC, 0, 0};
    IMPCell s1 = {DEV_ID_FS, 1, 0}, d1 = {DEV_ID_ENC, 1, 0};
    IMP_System_UnBind(&s0, &d0);
    IMP_System_UnBind(&s1, &d1);
    IMP_FrameSource_DisableChn(0);
    IMP_FrameSource_DisableChn(1);
    IMP_System_Exit();
    IMP_ISP_DisableSensor();
    IMP_ISP_DelSensor(&sensor);
    IMP_ISP_DisableTuning();
    IMP_ISP_Close();
    return 0;
}
