#ifndef UVC_PNG_H
#define UVC_PNG_H

#include <stddef.h>
#include <stdint.h>

// Encode an 8-bit grayscale image (w x h, no padding) as PNG into a
// malloc'd buffer. 0 on success.
int png_gray(const uint8_t *px, int w, int h, uint8_t **out, size_t *len);

#endif
