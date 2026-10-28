#ifndef UVC_CAPTURE_V4L2_H
#define UVC_CAPTURE_V4L2_H

#include "capture.h"

// capture.h's pipeline over our kernel's V4L2 drivers (t20-isp, jxf2x,
// t20-vpu) instead of libimp; capture.c picks it when they are there.
int capture_v4l2_available(void);
int capture_v4l2_start(const struct config *c, struct source *hi, struct source *lo);
void capture_v4l2_stop(void);
int capture_v4l2_jpeg(uint8_t **data, size_t *len);
int capture_v4l2_raw(uint8_t *buf, size_t cap, int *w, int *h, int chroma);
int capture_v4l2_exposure(void);
void capture_v4l2_isp(const struct isp_params *p);
void capture_v4l2_night(int on);

#endif
