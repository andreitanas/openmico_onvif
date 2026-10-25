#include "ws.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { OP_CONT = 0, OP_TEXT = 1, OP_BIN = 2, OP_CLOSE = 8, OP_PING = 9, OP_PONG = 10 };

static void random_bytes(uint8_t *p, size_t n) {
    FILE *fp = fopen("/dev/urandom", "rb");
    if (!fp || fread(p, 1, n, fp) != n)
        for (size_t i = 0; i < n; i++)
            p[i] = (uint8_t)(rand() ^ mono_ms());
    if (fp)
        fclose(fp);
}

int ws_connect(struct ws *w, const char *host, int port, const char *path, const char *headers) {
    memset(w, 0, sizeof(*w));
    if (tls_connect(&w->tls, host, port, 10000) < 0)
        return -1;

    uint8_t nonce[16];
    char key[32];
    random_bytes(nonce, sizeof(nonce));
    base64_encode(nonce, sizeof(nonce), key);
    char req[2048];
    int n = snprintf(req, sizeof(req),
                     "GET %s HTTP/1.1\r\n"
                     "Host: %s:%d\r\n"
                     "Upgrade: websocket\r\n"
                     "Connection: Upgrade\r\n"
                     "Sec-WebSocket-Key: %s\r\n"
                     "Sec-WebSocket-Version: 13\r\n"
                     "Sec-WebSocket-Protocol: secure_transfer\r\n"
                     "%s\r\n",
                     path, host, port, key, headers);
    if (n >= (int)sizeof(req) || tls_write_all(&w->tls, req, n) < 0)
        goto fail;

    // Read the HTTP response head; anything after it is websocket data.
    for (;;) {
        uint8_t chunk[1024];
        int r = tls_read(&w->tls, chunk, sizeof(chunk), 10000);
        if (r <= 0)
            goto fail;
        buf_put(&w->in, chunk, r);
        buf_u8(&w->in, 0);
        w->in.len--;
        char *end = strstr((char *)w->in.p, "\r\n\r\n");
        if (!end) {
            if (w->in.len > 16384)
                goto fail;
            continue;
        }
        int status = 0;
        sscanf((char *)w->in.p, "HTTP/1.1 %d", &status);
        if (status != 101) {
            *end = 0;
            log_warn("websocket upgrade refused: %s", (char *)w->in.p);
            goto fail;
        }
        buf_consume(&w->in, end + 4 - (char *)w->in.p);
        return 0;
    }
fail:
    ws_close(w);
    return -1;
}

static int send_frame(struct ws *w, int op, const uint8_t *data, size_t len) {
    uint8_t hdr[14];
    size_t h = 0;
    hdr[h++] = 0x80 | op;
    if (len < 126) {
        hdr[h++] = 0x80 | len;
    } else if (len < 65536) {
        hdr[h++] = 0x80 | 126;
        hdr[h++] = len >> 8;
        hdr[h++] = len;
    } else {
        hdr[h++] = 0x80 | 127;
        for (int i = 7; i >= 0; i--)
            hdr[h++] = (uint64_t)len >> (8 * i);
    }
    uint8_t mask[4];
    random_bytes(mask, 4);
    memcpy(hdr + h, mask, 4);
    h += 4;

    struct buf b = {0};
    buf_put(&b, hdr, h);
    buf_reserve(&b, len);
    for (size_t i = 0; i < len; i++)
        b.p[b.len + i] = data[i] ^ mask[i & 3];
    b.len += len;
    int rc = tls_write_all(&w->tls, b.p, b.len);
    buf_free(&b);
    return rc;
}

int ws_send_text(struct ws *w, const char *text, size_t len) {
    return send_frame(w, OP_TEXT, (const uint8_t *)text, len);
}

// Parse one complete frame from w->in. 1 = frame parsed, 0 = need more data.
static int parse_frame(struct ws *w, int *fin, int *op, size_t *start, size_t *plen) {
    uint8_t *p = w->in.p;
    size_t n = w->in.len;
    if (n < 2)
        return 0;
    *fin = p[0] >> 7;
    *op = p[0] & 15;
    int masked = p[1] >> 7;
    uint64_t len = p[1] & 127;
    size_t h = 2;
    if (len == 126) {
        if (n < 4)
            return 0;
        len = p[2] << 8 | p[3];
        h = 4;
    } else if (len == 127) {
        if (n < 10)
            return 0;
        len = 0;
        for (int i = 0; i < 8; i++)
            len = len << 8 | p[2 + i];
        h = 10;
    }
    size_t mask_at = h;
    if (masked)
        h += 4;
    if (n < h + len)
        return 0;
    if (masked)
        for (uint64_t i = 0; i < len; i++)
            p[h + i] ^= p[mask_at + (i & 3)];
    *start = h;
    *plen = len;
    return 1;
}

int ws_recv(struct ws *w, const char **out, size_t *len, int timeout_ms) {
    int64_t deadline = mono_ms() + timeout_ms;
    for (;;) {
        int fin, op;
        size_t start, plen;
        while (parse_frame(w, &fin, &op, &start, &plen)) {
            const uint8_t *payload = w->in.p + start;
            if (op == OP_PING) {
                send_frame(w, OP_PONG, payload, plen);
            } else if (op == OP_CLOSE) {
                if (w->peer_closed)
                    return -1;
                w->peer_closed = 1;
                log_info("websocket closed by peer (code %d)",
                         plen >= 2 ? payload[0] << 8 | payload[1] : 0);
                return -1;
            } else if (op == OP_TEXT || op == OP_BIN || op == OP_CONT) {
                if (op != OP_CONT)
                    w->msg.len = 0;
                buf_put(&w->msg, payload, plen);
                if (fin) {
                    buf_consume(&w->in, start + plen);
                    buf_u8(&w->msg, 0);
                    *out = (const char *)w->msg.p;
                    *len = w->msg.len - 1;
                    return 1;
                }
            }
            buf_consume(&w->in, start + plen);
        }

        int64_t left = deadline - mono_ms();
        if (left <= 0)
            return 0;
        uint8_t chunk[4096];
        int r = tls_read(&w->tls, chunk, sizeof(chunk), (int)left);
        if (r < 0)
            return -1;
        if (r == 0)
            return 0;
        buf_put(&w->in, chunk, r);
    }
}

void ws_close(struct ws *w) {
    if (w->tls.open) {
        // Our close starts the handshake (wait briefly for the peer's), or
        // answers the peer's.
        if (send_frame(w, OP_CLOSE, (const uint8_t *)"\x03\xe8", 2) == 0 && !w->peer_closed) {
            const char *text;
            size_t len;
            int64_t deadline = mono_ms() + 1000;
            while (mono_ms() < deadline && ws_recv(w, &text, &len, 200) >= 0)
                ;
        }
        tls_close(&w->tls);
    }
    buf_free(&w->in);
    buf_free(&w->msg);
    w->peer_closed = 0;
}
