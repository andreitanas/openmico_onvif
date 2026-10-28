// Hardware test of src/capture_v4l2.c on the camera (our kernel, uvcd
// stopped): both H.264 streams, JPEG snapshots, raw frames, the exposure
// figure, ISP settings, restarts for flip and mirror, and start/stop cycles.
// Writes /tmp/snap0.jpg (normal), /tmp/snap1.jpg (flipped), /tmp/snap2.jpg
// (mirrored) and /tmp/raw.nv12 (the low-resolution frame).
//   make build/test_capture_v4l2-mipsel; copy it over, stop uvcd, run it.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "capture_v4l2.h"
#include "util.h"

static int failures;

#define CHECK(cond, ...)                         \
    do {                                         \
        if (!(cond)) {                           \
            printf("FAIL %s: ", #cond);          \
            printf(__VA_ARGS__);                 \
            printf("\n");                        \
            failures++;                          \
        }                                        \
    } while (0)

// Frames of a source over ms milliseconds, and the keyframes among them
static void count(struct source *s, int ms, int *frames, int *keys) {
    struct queue q;
    queue_init(&q);
    source_subscribe(s, &q);
    *frames = *keys = 0;
    for (int64_t end = mono_ms() + ms; mono_ms() < end;) {
        struct frame *f = queue_pop(&q, 100);
        if (!f)
            continue;
        ++*frames;
        *keys += f->key;
        frame_unref(f);
    }
    source_unsubscribe(s, &q);
    queue_destroy(&q);
}

// A new subscriber starts at a keyframe: up to a GOP (hi 2 s, lo 1 s) is lost
static void streams(struct source *hi, struct source *lo, const char *when) {
    int hf, hk, lf, lk;
    count(hi, 4000, &hf, &hk);
    count(lo, 3000, &lf, &lk);
    printf("%s: hi %d frames (%d key) in 4 s, lo %d (%d key) in 3 s\n", when, hf, hk, lf, lk);
    CHECK(hf >= 45, "hi %d frames", hf);
    CHECK(hk >= 1, "hi no keyframe");
    CHECK(lf >= 45, "lo %d frames", lf);
}

static void snapshot(const char *path) {
    uint8_t *jpg;
    size_t len;
    int64_t t = mono_ms();
    int rc = capture_v4l2_jpeg(&jpg, &len);
    t = mono_ms() - t;
    CHECK(rc == 0, "no JPEG");
    if (rc)
        return;
    CHECK(len > 1000 && jpg[0] == 0xff && jpg[1] == 0xd8 && jpg[len - 2] == 0xff &&
              jpg[len - 1] == 0xd9,
          "not a JPEG (%zu bytes)", len);
    FILE *f = fopen(path, "wb");
    if (f) {
        fwrite(jpg, 1, len, f);
        fclose(f);
    }
    printf("%s: %zu bytes in %lld ms\n", path, len, (long long)t);
    free(jpg);
}

int main(void) {
    struct config c;
    memset(&c, 0, sizeof(c));
    c.fps = 25;
    c.hi_kbps = 2000;
    c.lo_kbps = 300;
    c.hi_gop = 50;
    c.lo_gop = 25;
    c.lo_width = 640;
    c.lo_height = 360;
    struct source hi, lo;
    source_init(&hi, "hi");
    source_init(&lo, "lo");

    CHECK(capture_v4l2_available(), "no t20-isp-main");
    CHECK(capture_v4l2_start(&c, &hi, &lo) == 0, "start");
    streams(&hi, &lo, "start");

    snapshot("/tmp/snap0.jpg");
    int64_t t = mono_ms();
    uint8_t *jpg;
    size_t len;
    CHECK(capture_v4l2_jpeg(&jpg, &len) == 0, "second JPEG");
    free(jpg);
    printf("second snapshot (cached) in %lld ms\n", (long long)(mono_ms() - t));

    static uint8_t raw[640 * 360 * 3 / 2];
    int w, h;
    t = mono_ms();
    int n = capture_v4l2_raw(raw, sizeof(raw), &w, &h, 1);
    printf("raw frame: %d bytes %dx%d in %lld ms\n", n, w, h, (long long)(mono_ms() - t));
    CHECK(n == (int)sizeof(raw) && w == 640 && h == 360, "raw %d", n);
    FILE *f = fopen("/tmp/raw.nv12", "wb");
    if (f) {
        fwrite(raw, 1, sizeof(raw), f);
        fclose(f);
    }
    printf("exposure figure %d\n", capture_v4l2_exposure());
    CHECK(capture_v4l2_exposure() > 0, "no exposure figure");

    struct isp_params p = {50, 60, 70, 50, 40, 0, 0};
    capture_v4l2_isp(&p);
    streams(&hi, &lo, "settings");

    p.flip = 1;
    capture_v4l2_isp(&p);
    streams(&hi, &lo, "flip");
    snapshot("/tmp/snap1.jpg");
    p.flip = 0;
    p.mirror = 1;
    capture_v4l2_isp(&p);
    streams(&hi, &lo, "mirror");
    snapshot("/tmp/snap2.jpg");
    p.mirror = 0;
    capture_v4l2_isp(&p);

    capture_v4l2_night(1);
    streams(&hi, &lo, "night");
    capture_v4l2_night(0);

    for (int i = 0; i < 3; i++) {
        capture_v4l2_stop();
        CHECK(capture_v4l2_start(&c, &hi, &lo) == 0, "restart %d", i);
    }
    streams(&hi, &lo, "after restarts");
    capture_v4l2_stop();
    CHECK(capture_v4l2_jpeg(&jpg, &len) < 0, "JPEG while stopped");

    printf(failures ? "%d FAILED\n" : "all passed\n", failures);
    return failures != 0;
}
