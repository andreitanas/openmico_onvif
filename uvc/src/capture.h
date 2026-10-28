#ifndef UVC_CAPTURE_H
#define UVC_CAPTURE_H

#include <stddef.h>
#include <stdint.h>

#include "config.h"
#include "source.h"

// The camera pipeline: sensor and ISP, a full-resolution H.264 encoder
// feeding hi, a low-resolution one feeding lo, an on-demand JPEG encoder,
// and raw low-resolution frames for motion detection. Through Ingenic's
// libimp (camera builds, 3.10 kernel) or our kernel's V4L2 drivers
// (capture_v4l2.c); either has a single owner process.
int capture_start(const struct config *c, struct source *hi, struct source *lo);
void capture_stop(void);

// A full-resolution JPEG at most ~0.5 s old, malloc'd. 0 on success.
int capture_jpeg(uint8_t **data, size_t *len);

// Copy the newest low-resolution frame: the luma plane, plus the interleaved
// chroma plane after it (NV12) if chroma is set. Returns bytes copied or -1.
int capture_raw(uint8_t *buf, size_t cap, int *w, int *h, int chroma);

// Image settings from Protect, 0-100 with 50 neutral, applied to the ISP.
// flip/mirror add to the config's FLIP_V/FLIP_H (mounting).
struct isp_params {
    int brightness, contrast, saturation, sharpness, denoise;
    int flip, mirror;
};
void capture_isp(const struct isp_params *p);

// The ISP's night mode (black and white, night tuning) or day mode.
void capture_night(int night);

// The exposure the scene needs, as the vendor ISP's "exposure log2 id"
// (log2(integration lines x gain) x 65536; daynight.h), or -1 if this
// pipeline does not give it (libimp: daynight reads /proc/jz/isp instead).
int capture_exposure(void);

#endif
