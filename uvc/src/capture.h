#ifndef UVC_CAPTURE_H
#define UVC_CAPTURE_H

#include <stddef.h>
#include <stdint.h>

#include "config.h"
#include "source.h"

// The camera pipeline through Ingenic's libimp (camera builds only): sensor
// and ISP, a full-resolution H.264 encoder feeding hi, a low-resolution one
// feeding lo, an on-demand JPEG encoder, and raw low-resolution frames for
// motion detection. Replaces OpenMiko's videocapture, which must not run
// (libimp has a single owner process).
int capture_start(const struct config *c, struct source *hi, struct source *lo);
void capture_stop(void);

// A full-resolution JPEG at most ~0.5 s old, malloc'd. 0 on success.
int capture_jpeg(uint8_t **data, size_t *len);

// Copy the newest low-resolution frame: the luma plane, plus the interleaved
// chroma plane after it (NV12) if chroma is set. Returns bytes copied or -1.
int capture_raw(uint8_t *buf, size_t cap, int *w, int *h, int chroma);

#endif
