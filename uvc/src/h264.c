#include "h264.h"

#include <string.h>

static size_t find_start(const uint8_t *buf, size_t size, size_t from, size_t *code_len) {
    for (size_t i = from; i + 3 <= size; i++) {
        if (buf[i] == 0 && buf[i + 1] == 0) {
            if (buf[i + 2] == 1) {
                *code_len = 3;
                return i;
            }
            if (i + 4 <= size && buf[i + 2] == 0 && buf[i + 3] == 1) {
                *code_len = 4;
                return i;
            }
        }
    }
    *code_len = 0;
    return size;
}

int h264_next_nal(const uint8_t *buf, size_t size, size_t *pos, const uint8_t **nal, size_t *len) {
    size_t code;
    size_t start = find_start(buf, size, *pos, &code);
    if (start >= size)
        return 0;
    start += code;
    size_t next_code;
    size_t end = find_start(buf, size, start, &next_code);
    // A 4-byte start code is a zero byte followed by a 3-byte one; the
    // search above already reports it as 4 bytes at its first zero.
    *nal = buf + start;
    *len = end - start;
    *pos = end;
    return *len > 0 || end < size ? 1 : 0;
}

int h264_is_key(const uint8_t *buf, size_t size) {
    size_t pos = 0, len;
    const uint8_t *nal;
    while (h264_next_nal(buf, size, &pos, &nal, &len))
        if (len && h264_type(nal) == NAL_IDR)
            return 1;
    return 0;
}

// Exp-Golomb bit reader over an RBSP (emulation prevention bytes removed).
struct bits {
    const uint8_t *p;
    size_t len, pos;
};

static int bit(struct bits *b) {
    if (b->pos >= b->len * 8)
        return 0;
    int v = (b->p[b->pos / 8] >> (7 - b->pos % 8)) & 1;
    b->pos++;
    return v;
}

static unsigned bits_n(struct bits *b, int n) {
    unsigned v = 0;
    while (n--)
        v = v << 1 | bit(b);
    return v;
}

static unsigned ue(struct bits *b) {
    int zeros = 0;
    while (!bit(b) && zeros < 32)
        zeros++;
    return ((1u << zeros) - 1) + bits_n(b, zeros);
}

static int se(struct bits *b) {
    unsigned v = ue(b);
    return v & 1 ? (int)((v + 1) / 2) : -(int)(v / 2);
}

static void skip_scaling_list(struct bits *b, int size) {
    int last = 8, next = 8;
    for (int i = 0; i < size; i++) {
        if (next != 0)
            next = (last + se(b) + 256) % 256;
        last = next == 0 ? last : next;
    }
}

int h264_sps_size(const uint8_t *sps, size_t len, int *width, int *height) {
    uint8_t rbsp[256];
    size_t n = 0;
    for (size_t i = 1; i < len && n < sizeof(rbsp); i++) {
        if (i >= 3 && sps[i] == 3 && sps[i - 1] == 0 && sps[i - 2] == 0)
            continue;
        rbsp[n++] = sps[i];
    }
    struct bits b = {rbsp, n, 0};
    unsigned profile = bits_n(&b, 8);
    bits_n(&b, 16); // constraint flags + level
    ue(&b);         // seq_parameter_set_id
    unsigned chroma = 1;
    if (profile == 100 || profile == 110 || profile == 122 || profile == 244 || profile == 44 ||
        profile == 83 || profile == 86 || profile == 118 || profile == 128) {
        chroma = ue(&b);
        if (chroma == 3)
            bit(&b);
        ue(&b); // bit_depth_luma
        ue(&b); // bit_depth_chroma
        bit(&b);
        if (bit(&b))
            for (int i = 0; i < (chroma != 3 ? 8 : 12); i++)
                if (bit(&b))
                    skip_scaling_list(&b, i < 6 ? 16 : 64);
    }
    ue(&b); // log2_max_frame_num
    unsigned poc_type = ue(&b);
    if (poc_type == 0) {
        ue(&b);
    } else if (poc_type == 1) {
        bit(&b);
        se(&b);
        se(&b);
        unsigned cycle = ue(&b);
        for (unsigned i = 0; i < cycle && i < 256; i++)
            se(&b);
    }
    ue(&b); // max_num_ref_frames
    bit(&b);
    unsigned mbs_w = ue(&b) + 1;
    unsigned map_h = ue(&b) + 1;
    unsigned frame_mbs_only = bit(&b);
    if (!frame_mbs_only)
        bit(&b);
    bit(&b); // direct_8x8_inference
    unsigned cl = 0, cr = 0, ct = 0, cb = 0;
    if (bit(&b)) {
        cl = ue(&b);
        cr = ue(&b);
        ct = ue(&b);
        cb = ue(&b);
    }
    unsigned sub_w = chroma == 1 || chroma == 2 ? 2 : 1;
    unsigned sub_h = chroma == 1 ? 2 : 1;
    if (chroma == 0)
        sub_w = sub_h = 1;
    unsigned h_mul = 2 - frame_mbs_only;
    *width = mbs_w * 16 - (cl + cr) * sub_w;
    *height = map_h * 16 * h_mul - (ct + cb) * sub_h * h_mul;
    return *width > 0 && *height > 0 && *width <= 8192 && *height <= 8192 ? 0 : -1;
}
