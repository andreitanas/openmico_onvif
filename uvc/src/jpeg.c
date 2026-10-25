#include "jpeg.h"

#include <stdlib.h>

#include "util.h"

// stb_image_write: public domain, used only for its JPEG writer.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#define STBI_WRITE_NO_STDIO
#define STB_IMAGE_WRITE_STATIC
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"
#pragma GCC diagnostic pop

static void append(void *ctx, void *data, int size) { buf_put(ctx, data, size); }

int jpeg_rgb(const uint8_t *rgb, int w, int h, int quality, uint8_t **out, size_t *len) {
    struct buf b = {0};
    if (!stbi_write_jpg_to_func(append, &b, w, h, 3, rgb, quality) || !b.len) {
        buf_free(&b);
        return -1;
    }
    *out = b.p;
    *len = b.len;
    return 0;
}
