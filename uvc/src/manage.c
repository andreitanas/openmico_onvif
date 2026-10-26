#define _GNU_SOURCE // strcasestr
#include "manage.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#include "cJSON.h"
#include "tls.h"
#include "util.h"

#define PORT "443"
#define MAX_REQUEST 8192

static const struct config *cfg;

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static struct { // guarded by lock
    int pending;
    char host[64];
    int port;
    char token[128];
} req;

int manage_pending(void) {
    pthread_mutex_lock(&lock);
    int p = req.pending;
    pthread_mutex_unlock(&lock);
    return p;
}

int manage_take(struct config *c) {
    pthread_mutex_lock(&lock);
    int p = req.pending;
    if (p)
        config_manage(c, req.host, req.port, req.token);
    req.pending = 0;
    pthread_mutex_unlock(&lock);
    if (p)
        log_info("adopting: Protect %s:%d", c->nvr, c->nvr_port);
    return p;
}

// The first "ip:port" of mgmt.hosts, and mgmt.token. 0 on success.
static int parse(const char *body, char *host, size_t host_len, int *port, char *token,
                 size_t token_len) {
    cJSON *j = cJSON_Parse(body);
    cJSON *mgmt = cJSON_GetObjectItem(j, "mgmt");
    const char *tok = cJSON_GetStringValue(cJSON_GetObjectItem(mgmt, "token"));
    const char *h = cJSON_GetStringValue(cJSON_GetArrayItem(cJSON_GetObjectItem(mgmt, "hosts"), 0));
    int rc = -1;
    if (tok && *tok && h && *h && strlen(tok) < token_len) {
        snprintf(host, host_len, "%s", h);
        *port = 7442;
        char *colon = strrchr(host, ':');
        if (colon) {
            *colon = 0;
            *port = atoi(colon + 1);
        }
        snprintf(token, token_len, "%s", tok);
        rc = *host && *port > 0 ? 0 : -1;
    }
    cJSON_Delete(j);
    return rc;
}

static int password_ok(const char *body) {
    cJSON *j = cJSON_Parse(body);
    const char *pw = cJSON_GetStringValue(cJSON_GetObjectItem(j, "password"));
    const char *hash = cfg->password_hash;
    const char *c = pw && *hash ? crypt(pw, hash) : NULL;
    int ok = c && !strcmp(c, hash);
    cJSON_Delete(j);
    return ok;
}

static void respond(struct tls *t, int status, const char *reason) {
    const char *body = status == 200 ? "{}" : "";
    char head[256];
    int n = snprintf(head, sizeof(head),
                     "HTTP/1.1 %d %s\r\nContent-Type: application/json\r\n"
                     "Content-Length: %zu\r\nConnection: close\r\n\r\n%s",
                     status, reason, strlen(body), body);
    tls_write_all(t, head, n);
}

// One request per connection; only POST .../manage is served.
static void serve(struct tls *t) {
    static char buf[MAX_REQUEST + 1];
    size_t len = 0;
    char *body = NULL;
    long need = -1;
    while (len < MAX_REQUEST) {
        int n = tls_read(t, buf + len, MAX_REQUEST - len, 5000);
        if (n <= 0)
            return;
        len += n;
        buf[len] = 0;
        if (!body && (body = strstr(buf, "\r\n\r\n"))) {
            body += 4;
            char *cl = strcasestr(buf, "\r\ncontent-length:");
            need = cl && cl < body ? atol(cl + 17) : 0;
        }
        if (body && (long)(len - (body - buf)) >= need)
            break;
    }
    if (!body || need < 0 || (long)(len - (body - buf)) < need) {
        respond(t, 400, "Bad Request");
        return;
    }
    char method[8] = "", path[64] = "";
    sscanf(buf, "%7s %63s", method, path);
    if (strcmp(method, "POST") || (strcmp(path, "/api/1.2/manage") && strcmp(path, "/api/1.1/manage"))) {
        log_info("manage: %s %s: not served", method, path);
        respond(t, 404, "Not Found");
        return;
    }
    // An adopted camera only takes a new host and token (Protect re-adopting
    // it) with the password Protect set: nobody else on the network can take
    // it over.
    if (cfg->adopted && !password_ok(body)) {
        log_warn("manage: adopt request with a wrong password, refused");
        respond(t, 403, "Forbidden");
        return;
    }
    char host[64], token[128];
    int port;
    if (parse(body, host, sizeof(host), &port, token, sizeof(token))) {
        log_warn("manage: adopt request without mgmt.hosts/token");
        respond(t, 400, "Bad Request");
        return;
    }
    pthread_mutex_lock(&lock);
    snprintf(req.host, sizeof(req.host), "%s", host);
    req.port = port;
    snprintf(req.token, sizeof(req.token), "%s", token);
    req.pending = 1;
    pthread_mutex_unlock(&lock);
    log_info("manage: adopt request for Protect %s:%d", host, port);
    respond(t, 200, "OK");
}

static void *manage_main(void *arg) {
    mbedtls_net_context listener;
    mbedtls_net_init(&listener);
    while (mbedtls_net_bind(&listener, NULL, PORT, MBEDTLS_NET_PROTO_TCP)) {
        log_warn("manage: cannot listen on port %s, retrying", PORT);
        sleep_ms(30000);
    }
    for (;;) {
        struct tls t;
        if (tls_accept(&t, &listener, 5000) == 0)
            serve(&t);
        tls_close(&t);
    }
    return NULL;
}

void manage_start(const struct config *c) {
    cfg = c;
    pthread_t thread;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    pthread_create(&thread, &attr, manage_main, NULL);
    pthread_attr_destroy(&attr);
}
