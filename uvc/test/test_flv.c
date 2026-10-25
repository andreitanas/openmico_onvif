// Replays an Annex-B clip through the real source + FLV muxer code and
// writes the UniFi FLV that would go to Protect.
//   test_flv clip.h264 out.flv seconds

#include <stdio.h>
#include <stdlib.h>

#include "flv.h"
#include "h264.h"
#include "source.h"
#include "util.h"

int main(int argc, char **argv) {
    if (argc != 4) {
        fprintf(stderr, "usage: %s clip.h264 out.flv seconds\n", argv[0]);
        return 2;
    }
    static struct source src;
    static struct queue q;
    queue_init(&q);
    source_start(&src, argv[1], 25);
    if (source_wait_ready(&src, 5000) < 0) {
        fprintf(stderr, "no SPS in %s\n", argv[1]);
        return 1;
    }
    source_subscribe(&src, &q);
    FILE *out = fopen(argv[2], "wb");
    struct flv flv;
    int started = 0;
    int64_t first = 0, end = mono_ms() + atoi(argv[3]) * 1000;
    while (mono_ms() < end) {
        struct frame *f = queue_pop(&q, 1000);
        if (!f)
            continue;
        if (!started) {
            const uint8_t *nal, *sps = NULL, *pps = NULL;
            size_t n, pos = 0, sl = 0, pl = 0;
            while (h264_next_nal(f->data, f->len, &pos, &nal, &n)) {
                if (h264_type(nal) == NAL_SPS)
                    sps = nal, sl = n;
                if (h264_type(nal) == NAL_PPS)
                    pps = nal, pl = n;
            }
            flv_begin(&flv, "testStream", src.width, src.height, 25, 1);
            flv_avc_config(&flv, sps, sl, pps, pl);
            first = f->ts_ms;
            started = 1;
        }
        flv_video(&flv, (uint32_t)(f->ts_ms - first), f->key, f->data, f->len);
        fwrite(flv.out.p, 1, flv.out.len, out);
        flv.out.len = 0;
        frame_unref(f);
    }
    fclose(out);
    printf("%dx%d\n", src.width, src.height);
    return 0;
}
