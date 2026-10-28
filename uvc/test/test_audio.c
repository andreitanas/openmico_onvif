// Hardware test of src/audio.c on the camera (our kernel): the microphone
// for a few seconds, the frames taken back by time as the FLV muxer does,
// written with ADTS headers to /tmp/audio.aac (ffmpeg plays that).
//   make build/test_audio-mipsel; copy it over, run it (uvcd may run: it
//   does not hold /dev/dsp unless audio is on)

#include <stdio.h>

#include "audio.h"
#include "util.h"

// ADTS for AAC-LC, 16 kHz (index 8), mono
static void adts(uint8_t *h, size_t len) {
    size_t n = len + 7;
    h[0] = 0xff;
    h[1] = 0xf1;
    h[2] = 1 << 6 | 8 << 2;
    h[3] = 1 << 6 | (n >> 11);
    h[4] = n >> 3;
    h[5] = (n & 7) << 5 | 0x1f;
    h[6] = 0xfc;
}

int main(void) {
    if (audio_start() < 0) {
        printf("FAIL: no microphone\n");
        return 1;
    }
    int64_t start = mono_ms();
    sleep_ms(5000);
    FILE *o = fopen("/tmp/audio.aac", "wb");
    int frames = 0, missing = 0;
    uint8_t aac[AUDIO_FRAME_MAX], h[7];
    // The last 3 s (the ring keeps 4), ending AUDIO_LATENCY_MS ago, in 64 ms steps
    for (int64_t t = mono_ms() - 3000; t < mono_ms() - AUDIO_LATENCY_MS; t += 64) {
        size_t n = audio_frame_at(t, aac);
        if (!n) {
            missing++;
            continue;
        }
        adts(h, n);
        fwrite(h, 1, 7, o);
        fwrite(aac, 1, n, o);
        frames++;
    }
    fclose(o);
    audio_stop();
    printf("%d frames, %d missing, %lld ms\n", frames, missing, (long long)(mono_ms() - start));
    printf(frames >= 40 && !missing ? "passed\n" : "FAILED\n");
    return !(frames >= 40 && !missing);
}
