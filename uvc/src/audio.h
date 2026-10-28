#ifndef UVC_AUDIO_H
#define UVC_AUDIO_H

#include <stddef.h>
#include <stdint.h>

// The microphone on our kernel (/dev/dsp from the T20's internal codec),
// encoded to AAC-LC, 16 kHz mono, 1024 samples (64 ms) a frame: the stream's
// audio track (flv.c), in place of silence. 0 if it started.
int audio_start(void);
void audio_stop(void);

// Protect's microphone volume, 0 (silence) to 100 (the most gain)
void audio_set_volume(int volume);

// The raw AAC frame (no ADTS header) captured at monotonic time t_ms: its
// length, or 0 if there is none (not yet encoded, gone from the buffer, or
// no microphone). out holds AUDIO_FRAME_MAX bytes.
#define AUDIO_FRAME_MAX 768
size_t audio_frame_at(int64_t t_ms, uint8_t *out);

// How late audio_frame_at has a frame: the muxer interleaves audio that
// far behind the video
#define AUDIO_LATENCY_MS 300

#endif
