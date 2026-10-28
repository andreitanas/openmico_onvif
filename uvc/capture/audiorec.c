// Hardware test of the microphone on our kernel (t20-codec, through the
// AIC and /dev/dsp): record 16-bit samples to a WAV file and print the
// level of each channel per second.
//   audiorec out.wav [seconds] [rate] [channels]
// Build: zig cc -target mipsel-linux-musleabihf -mcpu=mips32r2 -Os
//   -static -o audiorec capture/audiorec.c

#include <fcntl.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/soundcard.h>
#include <unistd.h>

static void put32(uint8_t *p, uint32_t v) {
    p[0] = v;
    p[1] = v >> 8;
    p[2] = v >> 16;
    p[3] = v >> 24;
}

static void put16(uint8_t *p, uint16_t v) {
    p[0] = v;
    p[1] = v >> 8;
}

static void wav_header(uint8_t *h, int rate, int channels, uint32_t bytes) {
    memcpy(h, "RIFF", 4);
    put32(h + 4, 36 + bytes);
    memcpy(h + 8, "WAVEfmt ", 8);
    put32(h + 16, 16);
    put16(h + 20, 1);  // PCM
    put16(h + 22, channels);
    put32(h + 24, rate);
    put32(h + 28, rate * channels * 2);
    put16(h + 32, channels * 2);
    put16(h + 34, 16);
    memcpy(h + 36, "data", 4);
    put32(h + 40, bytes);
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: %s out.wav [seconds] [rate] [channels]\n", argv[0]);
        return 2;
    }
    int seconds = argc > 2 ? atoi(argv[2]) : 5;
    int rate = argc > 3 ? atoi(argv[3]) : 16000;
    int channels = argc > 4 ? atoi(argv[4]) : 2;
    int fd = open("/dev/dsp", O_RDONLY);
    if (fd < 0) {
        perror("/dev/dsp");
        return 1;
    }
    int fmt = AFMT_S16_LE;
    if (ioctl(fd, SNDCTL_DSP_SETFMT, &fmt) < 0 || ioctl(fd, SNDCTL_DSP_CHANNELS, &channels) < 0 ||
        ioctl(fd, SNDCTL_DSP_SPEED, &rate) < 0) {
        perror("/dev/dsp setup");
        return 1;
    }
    printf("%d Hz, %d channels, format %#x\n", rate, channels, fmt);

    FILE *o = fopen(argv[1], "wb");
    if (!o) {
        perror(argv[1]);
        return 1;
    }
    uint8_t hdr[44];
    wav_header(hdr, rate, channels, 0);
    fwrite(hdr, 1, sizeof(hdr), o);

    int16_t *buf = malloc((size_t)rate * channels * 2);
    uint32_t total = 0;
    for (int s = 0; s < seconds; s++) {
        size_t want = (size_t)rate * channels * 2, got = 0;
        while (got < want) {
            ssize_t n = read(fd, (uint8_t *)buf + got, want - got);
            if (n <= 0) {
                perror("read");
                return 1;
            }
            got += n;
        }
        fwrite(buf, 1, got, o);
        total += got;
        printf("second %d:", s);
        for (int c = 0; c < channels; c++) {
            double sum = 0;
            int peak = 0;
            for (int i = c; i < rate * channels; i += channels) {
                sum += (double)buf[i] * buf[i];
                if (abs(buf[i]) > peak) peak = abs(buf[i]);
            }
            printf("  ch%d rms %.0f (%.1f dBFS) peak %d", c, sqrt(sum / rate),
                   20 * log10(sqrt(sum / rate) / 32768 + 1e-9), peak);
        }
        printf("\n");
    }
    wav_header(hdr, rate, channels, total);
    fseek(o, 0, SEEK_SET);
    fwrite(hdr, 1, sizeof(hdr), o);
    fclose(o);
    close(fd);
    return 0;
}
