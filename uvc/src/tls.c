#include "tls.h"

#include <stdio.h>
#include <string.h>

#include "mbedtls/error.h"
#include "mbedtls/x509_crt.h"
#include "psa/crypto.h"
#include "util.h"

static mbedtls_entropy_context entropy;
static mbedtls_ctr_drbg_context drbg;
static mbedtls_x509_crt cert;
static mbedtls_pk_context key;
static mbedtls_ssl_config conf, srv_conf;

static void tls_err(const char *what, int rc) {
    char msg[128];
    mbedtls_strerror(rc, msg, sizeof(msg));
    log_warn("%s: %s (-0x%04x)", what, msg, -rc);
}

int tls_init(const char *pem_path) {
    int rc;
    psa_crypto_init();
    mbedtls_entropy_init(&entropy);
    mbedtls_ctr_drbg_init(&drbg);
    if ((rc = mbedtls_ctr_drbg_seed(&drbg, mbedtls_entropy_func, &entropy,
                                    (const unsigned char *)"uvcd", 4))) {
        tls_err("drbg seed", rc);
        return -1;
    }
    mbedtls_x509_crt_init(&cert);
    mbedtls_pk_init(&key);
    // The PEM holds both the key and the certificate; each parser picks
    // out its own block.
    if ((rc = mbedtls_x509_crt_parse_file(&cert, pem_path)) < 0 || !cert.raw.len) {
        tls_err(pem_path, rc);
        return -1;
    }
    if ((rc = mbedtls_pk_parse_keyfile(&key, pem_path, NULL, mbedtls_ctr_drbg_random, &drbg))) {
        tls_err(pem_path, rc);
        return -1;
    }

    mbedtls_ssl_config_init(&conf);
    if ((rc = mbedtls_ssl_config_defaults(&conf, MBEDTLS_SSL_IS_CLIENT,
                                          MBEDTLS_SSL_TRANSPORT_STREAM,
                                          MBEDTLS_SSL_PRESET_DEFAULT))) {
        tls_err("ssl config", rc);
        return -1;
    }
    mbedtls_ssl_conf_authmode(&conf, MBEDTLS_SSL_VERIFY_NONE);
    mbedtls_ssl_conf_rng(&conf, mbedtls_ctr_drbg_random, &drbg);
    if ((rc = mbedtls_ssl_conf_own_cert(&conf, &cert, &key))) {
        tls_err("own cert", rc);
        return -1;
    }

    mbedtls_ssl_config_init(&srv_conf);
    if ((rc = mbedtls_ssl_config_defaults(&srv_conf, MBEDTLS_SSL_IS_SERVER,
                                          MBEDTLS_SSL_TRANSPORT_STREAM,
                                          MBEDTLS_SSL_PRESET_DEFAULT))) {
        tls_err("ssl server config", rc);
        return -1;
    }
    mbedtls_ssl_conf_rng(&srv_conf, mbedtls_ctr_drbg_random, &drbg);
    if ((rc = mbedtls_ssl_conf_own_cert(&srv_conf, &cert, &key))) {
        tls_err("own cert", rc);
        return -1;
    }
    return 0;
}

int tls_accept(struct tls *t, mbedtls_net_context *listener, int timeout_ms) {
    int rc;
    memset(t, 0, sizeof(*t));
    mbedtls_net_init(&t->net);
    mbedtls_ssl_init(&t->ssl);
    t->open = 1;
    t->server = 1;
    if ((rc = mbedtls_net_accept(listener, &t->net, NULL, 0, NULL))) {
        tls_err("accept", rc);
        goto fail;
    }
    if ((rc = mbedtls_ssl_setup(&t->ssl, &srv_conf))) {
        tls_err("ssl setup", rc);
        goto fail;
    }
    mbedtls_ssl_set_bio(&t->ssl, &t->net, mbedtls_net_send, NULL, mbedtls_net_recv_timeout);
    mbedtls_ssl_conf_read_timeout(&srv_conf, timeout_ms);
    while ((rc = mbedtls_ssl_handshake(&t->ssl))) {
        if (rc != MBEDTLS_ERR_SSL_WANT_READ && rc != MBEDTLS_ERR_SSL_WANT_WRITE) {
            tls_err("handshake", rc);
            goto fail;
        }
    }
    return 0;
fail:
    tls_close(t);
    return -1;
}

int tls_connect(struct tls *t, const char *host, int port, int timeout_ms) {
    char portstr[8];
    int rc;
    snprintf(portstr, sizeof(portstr), "%d", port);
    memset(t, 0, sizeof(*t));
    mbedtls_net_init(&t->net);
    mbedtls_ssl_init(&t->ssl);
    t->open = 1;
    if ((rc = mbedtls_net_connect(&t->net, host, portstr, MBEDTLS_NET_PROTO_TCP))) {
        tls_err(host, rc);
        goto fail;
    }
    if ((rc = mbedtls_ssl_setup(&t->ssl, &conf))) {
        tls_err("ssl setup", rc);
        goto fail;
    }
    mbedtls_ssl_set_bio(&t->ssl, &t->net, mbedtls_net_send, NULL, mbedtls_net_recv_timeout);
    mbedtls_ssl_conf_read_timeout(&conf, timeout_ms);
    while ((rc = mbedtls_ssl_handshake(&t->ssl))) {
        if (rc != MBEDTLS_ERR_SSL_WANT_READ && rc != MBEDTLS_ERR_SSL_WANT_WRITE) {
            tls_err("handshake", rc);
            goto fail;
        }
    }
    return 0;
fail:
    tls_close(t);
    return -1;
}

int tls_write_all(struct tls *t, const void *data, size_t len) {
    const unsigned char *p = data;
    while (len) {
        int n = mbedtls_ssl_write(&t->ssl, p, len);
        if (n == MBEDTLS_ERR_SSL_WANT_READ || n == MBEDTLS_ERR_SSL_WANT_WRITE)
            continue;
        if (n < 0) {
            tls_err("write", n);
            return -1;
        }
        p += n;
        len -= n;
    }
    return 0;
}

int tls_read(struct tls *t, void *buf, size_t len, int timeout_ms) {
    // The read timeout lives in the shared config; all reads happen on the
    // main thread (server ones on the manage thread) so this is not racy.
    mbedtls_ssl_conf_read_timeout(t->server ? &srv_conf : &conf, timeout_ms);
    for (;;) {
        int n = mbedtls_ssl_read(&t->ssl, buf, len);
        if (n > 0)
            return n;
        if (n == MBEDTLS_ERR_SSL_TIMEOUT)
            return 0;
        if (n == MBEDTLS_ERR_SSL_WANT_READ || n == MBEDTLS_ERR_SSL_WANT_WRITE ||
            n == MBEDTLS_ERR_SSL_RECEIVED_NEW_SESSION_TICKET)
            continue;
        if (n == 0 || n == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY)
            return -1;
        tls_err("read", n);
        return -1;
    }
}

void tls_close(struct tls *t) {
    if (!t->open)
        return;
    mbedtls_ssl_close_notify(&t->ssl);
    mbedtls_net_free(&t->net);
    mbedtls_ssl_free(&t->ssl);
    t->open = 0;
}
