#include "flv.h"

#include <stdlib.h>
#include <string.h>

#include "h264.h"

#define TAG_AUDIO 8
#define TAG_VIDEO 9
#define TAG_SCRIPT 18

#define SYNC_INTERVAL_MS 5000
#define AUDIO_RATE 16000
#define AAC_FRAME_SAMPLES 1024

// AAC-LC, 16 kHz, mono; and the silent raw frame ffmpeg's encoder emits.
static const uint8_t aac_config[] = {0x14, 0x08};
static const uint8_t aac_silence[] = {0x01, 0x18, 0x20, 0x07};

static void amf_key(struct buf *b, const char *key) {
    size_t n = strlen(key);
    buf_u16(b, n);
    buf_put(b, key, n);
}

static void amf_num(struct buf *b, const char *key, double v) {
    amf_key(b, key);
    buf_u8(b, 0x00);
    buf_f64(b, v);
}

static void amf_bool(struct buf *b, const char *key, int v) {
    amf_key(b, key);
    buf_u8(b, 0x01);
    buf_u8(b, v ? 1 : 0);
}

static void amf_str(struct buf *b, const char *key, const char *v) {
    amf_key(b, key);
    buf_u8(b, 0x02);
    amf_key(b, v);
}

static void amf_end(struct buf *b) { buf_u24(b, 9); }

// UniFi's inter-tag trailer: a zero byte, the 3-byte clock rate (90000 for
// video, 11025 for everything else), 8 zero bytes, then the elapsed wall
// clock in ticks of that clock. Protect's ingest silently drops the feed if
// these drift from real time.
static void trailer(struct flv *f, int video, int64_t now) {
    uint32_t clock = video ? 90000 : 11025;
    uint8_t t[16] = {0};
    put_u24(t + 1, clock);
    put_u32(t + 12, (uint32_t)((uint64_t)(now - f->start_ms) * clock / 1000));
    buf_put(&f->out, t, sizeof(t));
}

static void tag(struct flv *f, int type, uint32_t ts, const struct buf *body, int64_t now) {
    struct buf *o = &f->out;
    buf_reserve(o, 11 + body->len + 4 + 16);
    buf_u8(o, type);
    buf_u24(o, body->len);
    buf_u24(o, ts & 0xffffff);
    buf_u8(o, ts >> 24);
    buf_u24(o, 0);
    buf_put(o, body->p, body->len);
    buf_u32(o, body->len + 11);
    trailer(f, type == TAG_VIDEO, now);
}

static void script_name(struct buf *b, const char *name) {
    buf_u8(b, 0x02);
    amf_key(b, name);
}

static void bitrate_range(struct buf *b, const char *key) {
    amf_key(b, key);
    buf_u8(b, 0x03);
    amf_num(b, "cur", 1500000);
    amf_num(b, "max", 1500000);
    amf_num(b, "min", 1500000);
    amf_end(b);
}

// onClockSync ties the stream clock to wall time; onMpma carries bitrate
// stats a real camera reports. Same cadence and values as the proven
// unifi-cam-proxy clock_sync helper.
static void clock_sync(struct flv *f, uint32_t ts, int64_t now) {
    struct buf b = {0};
    script_name(&b, "onClockSync");
    buf_u8(&b, 0x03);
    amf_num(&b, "streamClock", ts);
    amf_num(&b, "streamClockBase", 0);
    amf_num(&b, "wallClock", (double)wall_ms());
    amf_end(&b);
    tag(f, TAG_SCRIPT, ts, &b, now);

    b.len = 0;
    script_name(&b, "onMpma");
    buf_u8(&b, 0x03);
    bitrate_range(&b, "cs");
    bitrate_range(&b, "m");
    amf_num(&b, "r", 0);
    bitrate_range(&b, "sp");
    amf_num(&b, "t", 75000.0);
    amf_end(&b);
    tag(f, TAG_SCRIPT, 0, &b, now);
    buf_free(&b);
}

void flv_begin(struct flv *f, const char *stream_name, int width, int height, int fps, int audio) {
    memset(f, 0, sizeof(*f));
    f->start_ms = f->last_sync_ms = mono_ms();
    f->audio = audio;

    static const uint8_t header[] = {'F', 'L', 'V', 1, 0x07, 0, 0, 0, 9, 0, 0, 0, 0};
    buf_put(&f->out, header, sizeof(header));

    struct buf b = {0};
    script_name(&b, "onMetaData");
    buf_u8(&b, 0x08);
    buf_u32(&b, audio ? 14 : 9);
    amf_num(&b, "duration", 0);
    amf_num(&b, "width", width);
    amf_num(&b, "height", height);
    amf_num(&b, "videodatarate", 0);
    amf_num(&b, "framerate", fps);
    amf_num(&b, "videocodecid", 7);
    if (audio) {
        amf_num(&b, "audiodatarate", 31.25);
        amf_num(&b, "audiosamplerate", AUDIO_RATE);
        amf_num(&b, "audiosamplesize", 16);
        amf_bool(&b, "stereo", 0);
        amf_num(&b, "audiocodecid", 10);
    }
    amf_str(&b, "streamName", stream_name);
    amf_str(&b, "encoder", "uvcd");
    amf_num(&b, "filesize", 0);
    amf_end(&b);
    int64_t now = mono_ms();
    tag(f, TAG_SCRIPT, 0, &b, now);
    buf_free(&b);
}

void flv_avc_config(struct flv *f, const uint8_t *sps, size_t sps_len, const uint8_t *pps,
                    size_t pps_len) {
    struct buf b = {0};
    static const uint8_t hdr[] = {0x17, 0, 0, 0, 0};
    buf_put(&b, hdr, sizeof(hdr));
    buf_u8(&b, 1);
    buf_put(&b, sps + 1, 3); // profile, compatibility, level
    buf_u8(&b, 0xff);        // 4-byte NAL lengths
    buf_u8(&b, 0xe1);        // one SPS
    buf_u16(&b, sps_len);
    buf_put(&b, sps, sps_len);
    buf_u8(&b, 1);
    buf_u16(&b, pps_len);
    buf_put(&b, pps, pps_len);
    int64_t now = mono_ms();
    tag(f, TAG_VIDEO, 0, &b, now);

    if (f->audio) {
        b.len = 0;
        buf_u8(&b, 0xaf);
        buf_u8(&b, 0);
        buf_put(&b, aac_config, sizeof(aac_config));
        tag(f, TAG_AUDIO, 0, &b, now);
    }
    buf_free(&b);
}

static void audio_until(struct flv *f, uint32_t ts, int64_t now) {
    struct buf b = {0};
    for (;;) {
        uint32_t ats = (uint32_t)(f->audio_samples * 1000 / AUDIO_RATE);
        if (ats > ts)
            break;
        b.len = 0;
        buf_u8(&b, 0xaf);
        buf_u8(&b, 1);
        buf_put(&b, aac_silence, sizeof(aac_silence));
        tag(f, TAG_AUDIO, ats, &b, now);
        f->audio_samples += AAC_FRAME_SAMPLES;
    }
    buf_free(&b);
}

void flv_video(struct flv *f, uint32_t ts, int key, const uint8_t *annexb, size_t len) {
    int64_t now = mono_ms();
    if (now - f->last_sync_ms >= SYNC_INTERVAL_MS) {
        f->last_sync_ms = now;
        clock_sync(f, ts, now);
    }
    if (f->audio)
        audio_until(f, ts, now);

    struct buf b = {0};
    buf_reserve(&b, len + 64);
    buf_u8(&b, key ? 0x17 : 0x27);
    buf_u8(&b, 1);
    buf_u24(&b, 0); // composition time: the T20 encoder emits no B-frames
    size_t pos = 0, n;
    const uint8_t *nal;
    while (h264_next_nal(annexb, len, &pos, &nal, &n)) {
        if (!n || h264_type(nal) == NAL_AUD)
            continue;
        buf_u32(&b, n);
        buf_put(&b, nal, n);
    }
    tag(f, TAG_VIDEO, ts, &b, now);
    buf_free(&b);
}

void flv_free(struct flv *f) { buf_free(&f->out); }
