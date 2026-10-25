#ifndef UVC_TLS_H
#define UVC_TLS_H

#include <stddef.h>

#include "mbedtls/ctr_drbg.h"
#include "mbedtls/entropy.h"
#include "mbedtls/net_sockets.h"
#include "mbedtls/ssl.h"

// Load the client certificate + key (one PEM file) used for every TLS
// connection to Protect. Protect does not validate our chain, and we do not
// validate its self-signed certificate.
int tls_init(const char *pem_path);

struct tls {
    mbedtls_net_context net;
    mbedtls_ssl_context ssl;
    int open;
};

int tls_connect(struct tls *t, const char *host, int port, int timeout_ms);
int tls_write_all(struct tls *t, const void *data, size_t len);
// >0 bytes read, 0 on timeout, <0 on error/close.
int tls_read(struct tls *t, void *buf, size_t len, int timeout_ms);
void tls_close(struct tls *t);

#endif
