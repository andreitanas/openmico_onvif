#include "png.h"

#include <stdlib.h>
#include <string.h>

#include "util.h"

static uint32_t crc32(uint32_t crc, const uint8_t *p, size_t n) {
    crc = ~crc;
    while (n--) {
        crc ^= *p++;
        for (int k = 0; k < 8; k++)
            crc = crc >> 1 ^ (0xedb88320 & -(crc & 1));
    }
    return ~crc;
}

static void chunk(struct buf *b, const char *type, const uint8_t *data, size_t len) {
    buf_u32(b, len);
    size_t start = b->len;
    buf_put(b, type, 4);
    if (len)
        buf_put(b, data, len);
    buf_u32(b, crc32(0, b->p + start, len + 4));
}

// zlib stream of "stored" (uncompressed) deflate blocks: tiny images only,
// and no compressor needed.
static void zlib_stored(struct buf *z, const uint8_t *raw, size_t len) {
    buf_u8(z, 0x78);
    buf_u8(z, 0x01);
    uint32_t a = 1, b = 0;
    for (size_t i = 0; i < len; i++) {
        a = (a + raw[i]) % 65521;
        b = (b + a) % 65521;
    }
    size_t pos = 0;
    do {
        size_t n = len - pos > 65535 ? 65535 : len - pos;
        buf_u8(z, pos + n == len);
        uint8_t lens[4] = {n & 0xff, n >> 8, ~n & 0xff, (~n >> 8) & 0xff};
        buf_put(z, lens, 4);
        buf_put(z, raw + pos, n);
        pos += n;
    } while (pos < len);
    buf_u32(z, b << 16 | a);
}

int png_gray(const uint8_t *px, int w, int h, uint8_t **out, size_t *len) {
    size_t raw_len = (size_t)(w + 1) * h;
    uint8_t *raw = malloc(raw_len);
    if (!raw)
        return -1;
    for (int y = 0; y < h; y++) {
        raw[y * (w + 1)] = 0; // filter: none
        memcpy(raw + y * (w + 1) + 1, px + y * w, w);
    }
    struct buf z = {0}, b = {0};
    zlib_stored(&z, raw, raw_len);
    free(raw);

    static const uint8_t sig[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'};
    buf_put(&b, sig, 8);
    uint8_t ihdr[13] = {0};
    put_u32(ihdr, w);
    put_u32(ihdr + 4, h);
    ihdr[8] = 8; // bit depth; colour type 0 (gray), default methods
    chunk(&b, "IHDR", ihdr, 13);
    chunk(&b, "IDAT", z.p, z.len);
    chunk(&b, "IEND", NULL, 0);
    buf_free(&z);
    *out = b.p;
    *len = b.len;
    return 0;
}
