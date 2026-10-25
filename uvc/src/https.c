#include "https.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tls.h"
#include "util.h"

static int parse_url(const char *url, char *host, size_t hlen, int *port, const char **path) {
    if (strncmp(url, "https://", 8))
        return -1;
    const char *h = url + 8;
    const char *slash = strchr(h, '/');
    *path = slash ? slash : "/";
    size_t n = slash ? (size_t)(slash - h) : strlen(h);
    if (n >= hlen)
        return -1;
    memcpy(host, h, n);
    host[n] = 0;
    char *colon = strchr(host, ':');
    *port = 443;
    if (colon) {
        *colon = 0;
        *port = atoi(colon + 1);
    }
    return 0;
}

int https_post_file(const char *url, const struct form_field *fields, int nfields,
                    const char *filename, const uint8_t *data, size_t len) {
    char host[128];
    int port;
    const char *path;
    if (parse_url(url, host, sizeof(host), &port, &path) < 0) {
        log_warn("unsupported upload URL %s", url);
        return -1;
    }

    char boundary[40];
    snprintf(boundary, sizeof(boundary), "uvcd%08lx%08lx", (unsigned long)wall_ms(),
             (unsigned long)rand());
    struct buf body = {0};
    char part[512];
    for (int i = 0; i < nfields; i++) {
        int n = snprintf(part, sizeof(part),
                         "--%s\r\nContent-Disposition: form-data; name=\"%s\"\r\n\r\n%s\r\n",
                         boundary, fields[i].name, fields[i].value);
        buf_put(&body, part, n);
    }
    int n = snprintf(part, sizeof(part),
                     "--%s\r\nContent-Disposition: form-data; name=\"payload\"; "
                     "filename=\"%s\"\r\nContent-Type: image/jpeg\r\n\r\n",
                     boundary, filename);
    buf_put(&body, part, n);
    buf_put(&body, data, len);
    n = snprintf(part, sizeof(part), "\r\n--%s--\r\n", boundary);
    buf_put(&body, part, n);

    char head[1024];
    int hn = snprintf(head, sizeof(head),
                      "POST %s HTTP/1.1\r\nHost: %s:%d\r\n"
                      "Content-Type: multipart/form-data; boundary=%s\r\n"
                      "Content-Length: %zu\r\nConnection: close\r\n\r\n",
                      path, host, port, boundary, body.len);

    int status = -1;
    struct tls t;
    if (hn < (int)sizeof(head) && tls_connect(&t, host, port, 5000) == 0) {
        if (tls_write_all(&t, head, hn) == 0 && tls_write_all(&t, body.p, body.len) == 0) {
            char resp[256];
            size_t got = 0;
            int64_t deadline = mono_ms() + 5000;
            while (got < 12 && mono_ms() < deadline) {
                int r = tls_read(&t, resp + got, sizeof(resp) - 1 - got, 1000);
                if (r < 0)
                    break;
                got += r;
            }
            resp[got] = 0;
            sscanf(resp, "HTTP/1.%*d %d", &status);
        }
        tls_close(&t);
    }
    buf_free(&body);
    return status;
}
