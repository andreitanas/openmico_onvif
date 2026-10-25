#ifndef UVC_H264_H
#define UVC_H264_H

#include <stddef.h>
#include <stdint.h>

enum {
    NAL_SLICE = 1,
    NAL_IDR = 5,
    NAL_SEI = 6,
    NAL_SPS = 7,
    NAL_PPS = 8,
    NAL_AUD = 9,
};

// Iterates the NAL units of an Annex-B buffer. Start with *pos = 0; returns
// 0 at the end. On success nal/len point at the unit without its start code.
int h264_next_nal(const uint8_t *buf, size_t size, size_t *pos, const uint8_t **nal, size_t *len);

static inline int h264_type(const uint8_t *nal) { return nal[0] & 0x1f; }

// Width/height from an SPS NAL (including its header byte). 0 on success.
int h264_sps_size(const uint8_t *sps, size_t len, int *width, int *height);

// True if the Annex-B buffer contains an IDR slice.
int h264_is_key(const uint8_t *buf, size_t size);

#endif
