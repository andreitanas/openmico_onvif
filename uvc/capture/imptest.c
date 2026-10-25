// Feasibility test: drive the T20 ISP through libimp from a static musl
// binary and grab raw NV12 frames from a scaled frame source channel.
// Run with OpenMiko's videocapture stopped (libimp allows one owner).
//   imptest out.nv12 [frames]

#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "imp_common.h"
#include "imp_framesource.h"
#include "imp_isp.h"
#include "imp_system.h"

#define W 640
#define H 360

static double now(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

#define CHECK(call)                                                   \
    do {                                                              \
        int rc_ = (call);                                             \
        printf("%-55s %s\n", #call, rc_ < 0 ? "FAILED" : "ok");      \
        if (rc_ < 0)                                                  \
            return 1;                                                 \
    } while (0)

// Print where a crash happened (MIPS sigcontext: pc at +8, 64-bit regs from
// +16), to symbolize against the unstripped binary.
static void on_crash(int sig, siginfo_t *si, void *ctx) {
    const unsigned char *mc = (const unsigned char *)&((ucontext_t *)ctx)->uc_mcontext;
    uint64_t pc, ra;
    memcpy(&pc, mc + 8, 8);
    memcpy(&ra, mc + 16 + 31 * 8, 8);
    fprintf(stderr, "signal %d at pc=0x%08x ra=0x%08x addr=%p\n", sig, (unsigned)pc, (unsigned)ra,
            si->si_addr);
    _exit(128 + sig);
}

int main(int argc, char **argv) {
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = on_crash;
    sa.sa_flags = SA_SIGINFO;
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGBUS, &sa, NULL);
    sigaction(SIGILL, &sa, NULL);
    setvbuf(stdout, NULL, _IONBF, 0);
    const char *out = argc > 1 ? argv[1] : "/tmp/frame.nv12";
    int frames = argc > 2 ? atoi(argv[2]) : 50;

    // Same probing as OpenMiko's S60camera/videocapture: the sensor name is
    // in /proc/jz/sinfo/info as "sensor :jxf23".
    char line[64] = {0}, *name;
    FILE *fp = fopen("/proc/jz/sinfo/info", "r");
    if (!fp || !fgets(line, sizeof(line), fp) || !(name = strchr(line, ':'))) {
        fprintf(stderr, "cannot read sensor name\n");
        return 1;
    }
    fclose(fp);
    name++;
    name[strcspn(name, "\n")] = 0;
    printf("sensor %s\n", name);

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

    IMPFSChnAttr attr;
    memset(&attr, 0, sizeof(attr));
    attr.pixFmt = PIX_FMT_NV12;
    attr.outFrmRateNum = 25;
    attr.outFrmRateDen = 1;
    attr.nrVBs = 2;
    attr.type = FS_PHY_CHANNEL;
    attr.scaler.enable = 1;
    attr.scaler.outwidth = W;
    attr.scaler.outheight = H;
    attr.picWidth = W;
    attr.picHeight = H;
    CHECK(IMP_FrameSource_CreateChn(0, &attr));
    CHECK(IMP_FrameSource_SetChnAttr(0, &attr));
    CHECK(IMP_FrameSource_EnableChn(0));
    CHECK(IMP_FrameSource_SetFrameDepth(0, 1)); // only valid once enabled

    double t0 = now(), first = 0;
    long sum = 0;
    int got = 0;
    for (int i = 0; i < frames; i++) {
        IMPFrameInfo *f;
        if (IMP_FrameSource_GetFrame(0, &f) < 0) {
            printf("GetFrame %d failed\n", i);
            continue;
        }
        if (!got++)
            first = now() - t0;
        const unsigned char *y = (const unsigned char *)f->virAddr;
        for (int k = 0; k < W * H; k += 97)
            sum += y[k];
        if (i == frames - 1) {
            FILE *o = fopen(out, "wb");
            fwrite(y, 1, W * H * 3 / 2, o);
            fclose(o);
            printf("frame %ux%u fmt %u size %u ts %lld -> %s\n", f->width, f->height, f->pixfmt,
                   f->size, (long long)f->timeStamp, out);
        }
        IMP_FrameSource_ReleaseFrame(0, f);
    }
    double dt = now() - t0;
    printf("%d frames in %.2f s (%.1f fps, first after %.2f s), mean luma %ld\n", got, dt,
           got / dt, first, got ? sum / (got * (W * H / 97 + 1)) : 0);

    IMP_FrameSource_DisableChn(0);
    IMP_FrameSource_DestroyChn(0);
    IMP_System_Exit();
    IMP_ISP_DisableSensor();
    IMP_ISP_DelSensor(&sensor);
    IMP_ISP_DisableTuning();
    IMP_ISP_Close();
    return 0;
}
