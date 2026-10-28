// The microphone: /dev/dsp (OSS emulation over the T20 codec's ALSA driver)
// at 16 kHz, 16-bit stereo, of which the right channel has the codec's one
// ADC; high-passed and amplified here (the codec's own gain controls change
// nothing: docs/t20-isp-hardware.md); AAC-LC from vo-aacenc (Apache-2.0); a
// ring of the last 4 s of frames, each placed in monotonic time by the
// samples read so far.

#include "audio.h"

#include "util.h"

#if defined(__linux__)

#include <fcntl.h>
#include <math.h>
#include <pthread.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/soundcard.h>
#include <unistd.h>

#include "cmnMemory.h"
#include "voAAC.h"

#define RATE 16000
#define FRAME 1024  // samples an AAC frame
#define FRAME_MS (FRAME * 1000 / RATE)
#define RING 64     // frames kept: 4 s
#define BITRATE 32000
#define HIGHPASS_HZ 120
#define GAIN_MAX_DB 30  // at Protect's microphone volume 100

// The ADC's output has a DC offset and the room's rumble below 100 Hz far
// above everything else: a 4th-order Butterworth high-pass, two biquads
struct biquad {
    float b0, b1, b2, a1, a2, z1, z2;
};

static struct biquad hp[2];
static volatile int volume = 100;

struct aframe {
    int64_t index;  // frame number since the start, -1: empty
    size_t len;
    uint8_t data[AUDIO_FRAME_MAX];
};

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static struct aframe ring[RING];
static int64_t t0_ms;  // monotonic time of sample 0
static pthread_t thread;
static volatile int running;
static int dsp = -1;

static VO_AUDIO_CODECAPI api;
static VO_HANDLE enc;
static VO_MEM_OPERATOR mem = {.Alloc = cmnMemAlloc, .Free = cmnMemFree, .Set = cmnMemSet,
                              .Copy = cmnMemCopy, .Check = cmnMemCheck};

static void highpass_init(void) {
    static const float q[2] = {0.5412f, 1.3066f};
    float w = 2 * (float)M_PI * HIGHPASS_HZ / RATE, c = cosf(w);
    for (int i = 0; i < 2; i++) {
        float alpha = sinf(w) / (2 * q[i]), a0 = 1 + alpha;
        hp[i] = (struct biquad){.b0 = (1 + c) / 2 / a0, .b1 = -(1 + c) / a0, .b2 = (1 + c) / 2 / a0,
                                .a1 = -2 * c / a0, .a2 = (1 - alpha) / a0};
    }
}

static int16_t process(int16_t in, float gain) {
    float x = in;
    for (int i = 0; i < 2; i++) {
        struct biquad *f = &hp[i];
        float y = f->b0 * x + f->z1;
        f->z1 = f->b1 * x - f->a1 * y + f->z2;
        f->z2 = f->b2 * x - f->a2 * y;
        x = y;
    }
    x *= gain;
    return x > 32767 ? 32767 : x < -32768 ? -32768 : (int16_t)lrintf(x);
}

void audio_set_volume(int v) { volume = v < 0 ? 0 : v > 100 ? 100 : v; }

static int encoder_open(void) {
    VO_CODEC_INIT_USERDATA user = {.memflag = VO_IMF_USERMEMOPERATOR, .memData = &mem};
    AACENC_PARAM p = {.sampleRate = RATE, .bitRate = BITRATE, .nChannels = 1, .adtsUsed = 0};
    if (voGetAACEncAPI(&api) != VO_ERR_NONE || api.Init(&enc, VO_AUDIO_CodingAAC, &user) != VO_ERR_NONE)
        return -1;
    if (api.SetParam(enc, VO_PID_AAC_ENCPARAM, &p) != VO_ERR_NONE) {
        api.Uninit(enc);
        return -1;
    }
    return 0;
}

static size_t encode(int16_t *pcm, uint8_t *out) {
    VO_CODECBUFFER in = {.Buffer = (uint8_t *)pcm, .Length = FRAME * 2}, o = {.Buffer = out,
                                                                                .Length = AUDIO_FRAME_MAX};
    VO_AUDIO_OUTPUTINFO info = {0};
    api.SetInputData(enc, &in);
    if (api.GetOutputData(enc, &o, &info) != VO_ERR_NONE)
        return 0;
    return o.Length;
}

static void *audio_main(void *arg) {
    static int16_t stereo[FRAME * 2], mono[FRAME];
    int64_t index = 0;
    while (running) {
        size_t got = 0;
        while (got < sizeof(stereo) && running) {
            ssize_t n = read(dsp, (uint8_t *)stereo + got, sizeof(stereo) - got);
            if (n <= 0) {
                log_warn("audio: read failed, microphone stopped");
                running = 0;
                break;
            }
            got += n;
        }
        if (!running)
            break;
        // The last sample was taken about now: the frame's start follows.
        // Smoothed, as reads come back late by a varying amount.
        int64_t start = mono_ms() - (index + 1) * FRAME_MS;
        int v = volume;
        float gain = v ? powf(10, GAIN_MAX_DB * v / 100.0f / 20) : 0;
        for (int i = 0; i < FRAME; i++)
            mono[i] = process(stereo[2 * i + 1], gain);
        struct aframe f = {.index = index};
        f.len = encode(mono, f.data);
        pthread_mutex_lock(&lock);
        t0_ms = index ? t0_ms + (start - t0_ms) / 32 : start;
        if (f.len)
            ring[index % RING] = f;
        pthread_mutex_unlock(&lock);
        index++;
    }
    return NULL;
}

int audio_start(void) {
    if (running)
        return 0;
    if ((dsp = open("/dev/dsp", O_RDONLY | O_CLOEXEC)) < 0)
        return -1;
    int fmt = AFMT_S16_LE, ch = 2, rate = RATE;
    // Four 4 KiB fragments: 64 ms of samples a read
    int frag = 4 << 16 | 12;
    ioctl(dsp, SNDCTL_DSP_SETFRAGMENT, &frag);
    if (ioctl(dsp, SNDCTL_DSP_SETFMT, &fmt) < 0 || fmt != AFMT_S16_LE ||
        ioctl(dsp, SNDCTL_DSP_CHANNELS, &ch) < 0 || ch != 2 ||
        ioctl(dsp, SNDCTL_DSP_SPEED, &rate) < 0 || rate != RATE || encoder_open() < 0) {
        log_warn("audio: no 16 kHz 16-bit microphone, the streams carry silence");
        close(dsp);
        dsp = -1;
        return -1;
    }
    for (int i = 0; i < RING; i++)
        ring[i].index = -1;
    highpass_init();
    running = 1;
    pthread_create(&thread, NULL, audio_main, NULL);
    log_info("audio: microphone, AAC-LC %d Hz %d kbit/s", RATE, BITRATE / 1000);
    return 0;
}

void audio_stop(void) {
    if (dsp < 0)
        return;
    running = 0;
    pthread_join(thread, NULL);
    api.Uninit(enc);
    close(dsp);
    dsp = -1;
    pthread_mutex_lock(&lock);
    for (int i = 0; i < RING; i++)
        ring[i].index = -1;
    pthread_mutex_unlock(&lock);
}

size_t audio_frame_at(int64_t t_ms, uint8_t *out) {
    size_t len = 0;
    pthread_mutex_lock(&lock);
    if (dsp >= 0 && t_ms >= t0_ms) {
        int64_t index = (t_ms - t0_ms) / FRAME_MS;
        struct aframe *f = &ring[index % RING];
        if (f->index == index) {
            memcpy(out, f->data, f->len);
            len = f->len;
        }
    }
    pthread_mutex_unlock(&lock);
    return len;
}

#else  // no OSS on this host

int audio_start(void) { return -1; }
void audio_set_volume(int volume) {}
void audio_stop(void) {}
size_t audio_frame_at(int64_t t_ms, uint8_t *out) { return 0; }

#endif
