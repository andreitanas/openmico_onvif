#ifndef UVC_UTIL_H
#define UVC_UTIL_H

#include <stddef.h>
#include <stdint.h>

enum { LOG_ERR, LOG_WARN, LOG_INFO, LOG_DEBUG };
extern int log_level;
// Log to path instead of stderr, rotating it to path.1 past max_bytes.
void log_open(const char *path, long max_bytes);
void log_msg(int level, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
#define log_error(...) log_msg(LOG_ERR, __VA_ARGS__)
#define log_warn(...) log_msg(LOG_WARN, __VA_ARGS__)
#define log_info(...) log_msg(LOG_INFO, __VA_ARGS__)
#define log_debug(...) log_msg(LOG_DEBUG, __VA_ARGS__)

int64_t mono_ms(void);
int64_t wall_ms(void);
void sleep_ms(int ms);

// Growable byte buffer with big-endian writers (FLV/AMF are big-endian).
struct buf {
    uint8_t *p;
    size_t len, cap;
};
void buf_reserve(struct buf *b, size_t extra);
void buf_put(struct buf *b, const void *data, size_t len);
void buf_u8(struct buf *b, uint8_t v);
void buf_u16(struct buf *b, uint16_t v);
void buf_u24(struct buf *b, uint32_t v);
void buf_u32(struct buf *b, uint32_t v);
void buf_f64(struct buf *b, double v);
void buf_consume(struct buf *b, size_t n);
void buf_free(struct buf *b);

void put_u24(uint8_t *p, uint32_t v);
void put_u32(uint8_t *p, uint32_t v);

void base64_encode(const uint8_t *in, size_t len, char *out);
int write_all(int fd, const void *data, size_t len);
int tcp_connect(const char *host, int port, int timeout_ms);

#endif
