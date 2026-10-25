#ifndef UVC_FLV_H
#define UVC_FLV_H

#include "util.h"

// FLV muxer producing the variant UniFi Protect ingests on port 7550:
// standard FLV tags, header flags 0x07, a 16-byte wall-clock trailer after
// every tag's PreviousTagSize, and onClockSync/onMpma script tags every 5 s.
struct flv {
    struct buf out; // bytes ready to send; caller drains it
    int64_t start_ms;
    int64_t last_sync_ms;
    int audio;              // interleave silent AAC so the stream has an audio track
    uint64_t audio_samples; // samples of silence emitted so far
};

void flv_begin(struct flv *f, const char *stream_name, int width, int height, int fps, int audio);
// SPS/PPS without start codes.
void flv_avc_config(struct flv *f, const uint8_t *sps, size_t sps_len, const uint8_t *pps,
                    size_t pps_len);
// One access unit in Annex-B form. ts in ms from the stream's first frame.
void flv_video(struct flv *f, uint32_t ts, int key, const uint8_t *annexb, size_t len);
void flv_free(struct flv *f);

#endif
