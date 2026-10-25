#ifndef UVC_SOURCE_H
#define UVC_SOURCE_H

#include <pthread.h>
#include <stddef.h>
#include <stdint.h>

// One encoded access unit (Annex-B), shared by reference between streams.
struct frame {
    int refs;
    int key;
    int64_t ts_ms; // monotonic capture time
    size_t len;
    uint8_t data[];
};

void frame_ref(struct frame *f);
void frame_unref(struct frame *f);

#define QUEUE_CAP 64

// Bounded frame queue between the source thread and one consumer. When the
// consumer falls behind, frames are dropped up to the next keyframe.
struct queue {
    pthread_mutex_t lock;
    pthread_cond_t cond;
    struct frame *items[QUEUE_CAP];
    int head, count;
    int need_key;
    struct queue *next; // source's subscriber list
};

void queue_init(struct queue *q);
void queue_destroy(struct queue *q);
// Next frame or NULL after timeout_ms. Caller owns the returned reference.
struct frame *queue_pop(struct queue *q, int timeout_ms);

struct source {
    char path[128];
    int fps;  // pacing for file sources
    int file; // path is a regular file of Annex-B H.264, looped
    pthread_t thread;
    pthread_mutex_t lock;
    struct queue *subs;
    int width, height; // from the SPS, 0 until the first keyframe
    int64_t last_frame_ms;
};

int source_start(struct source *s, const char *path, int fps);
void source_subscribe(struct source *s, struct queue *q);
void source_unsubscribe(struct source *s, struct queue *q);
// Wait until the stream geometry is known. 0 on success.
int source_wait_ready(struct source *s, int timeout_ms);

// Read a single JPEG from a v4l2loopback device or a file into a malloc'd
// buffer. 0 on success.
int read_jpeg(const char *path, uint8_t **data, size_t *len);

#endif
