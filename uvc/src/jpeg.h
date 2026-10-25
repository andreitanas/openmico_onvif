#ifndef UVC_JPEG_H
#define UVC_JPEG_H

#include <stddef.h>
#include <stdint.h>

// Encode packed RGB (w x h) as a baseline JPEG into a malloc'd buffer.
// 0 on success.
int jpeg_rgb(const uint8_t *rgb, int w, int h, int quality, uint8_t **out, size_t *len);

#endif
