#ifndef UVC_WS_H
#define UVC_WS_H

#include "tls.h"
#include "util.h"

// Minimal websocket client over TLS: text messages, ping/pong, close.
// Not thread-safe; the protocol loop owns it.
struct ws {
    struct tls tls;
    struct buf in;  // raw bytes received, not yet parsed
    struct buf msg; // reassembly of fragmented messages
};

// headers: extra "Name: value\r\n" lines.
int ws_connect(struct ws *w, const char *host, int port, const char *path, const char *headers);
int ws_send_text(struct ws *w, const char *text, size_t len);
// 1 = message in *out (NUL-terminated, valid until next call), 0 = timeout,
// -1 = connection closed or failed.
int ws_recv(struct ws *w, const char **out, size_t *len, int timeout_ms);
void ws_close(struct ws *w);

#endif
