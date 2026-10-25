#ifndef UVC_STREAM_H
#define UVC_STREAM_H

#include <pthread.h>

#include "source.h"

// Pushes one Protect video channel (video1/2/3) as UniFi FLV to the
// destination Protect named in ChangeVideoSettings.
struct stream {
    const char *channel; // "video1"...
    struct source *src;
    int fps;
    int audio;
    char name[96]; // Protect's streamName
    char host[64];
    int port;
    pthread_t thread;
    int running;
    volatile int stop;
    struct queue q;
};

void stream_start(struct stream *s, const char *name, const char *host, int port);
void stream_stop(struct stream *s);
// Same destination and name: nothing to do.
int stream_matches(const struct stream *s, const char *name, const char *host, int port);

#endif
