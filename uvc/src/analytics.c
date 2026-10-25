#include "analytics.h"

#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#include "capture.h"
#include "motion.h"
#include "objects.h"
#include "png.h"
#include "util.h"

#define FRAME_MS 200 // 5 frames a second is plenty for motion
#define SNAPSHOT_NAME "motionsnap.jpg"
#define HEATMAP_NAME "heatmap.png"

static analytics_send_fn send_event;
static int max_w, max_h;

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static struct motion_settings settings; // guarded by lock
static int settings_changed;            // guarded by lock
static uint8_t *snap, *heat;            // guarded by lock: the last event's files
static size_t snap_len, heat_len;
static int snap_w = 1920, snap_h = 1080; // the full-resolution JPEG

static struct motion motion; // detector thread only
static uint32_t event_id;
static int event_level;

#define NUM(o, k, v) cJSON_AddNumberToObject(o, k, v)
#define STR(o, k, v) cJSON_AddStringToObject(o, k, v)

// EventSmartMotion, the motion event of Gen5 cameras. Protect 7.3's device
// service (ds) parses it into CameraMotionEventPayload and drops anything
// else (older cameras' EventAnalytics included); field names are from its
// binary. Protect places the event by clockBestWall; the motion's start is
// backdated to its first frame, which was start_ms on the monotonic clock.
static cJSON *event_payload(const char *edge, int64_t start_ms) {
    int64_t now = mono_ms(), wall = wall_ms();
    cJSON *p = cJSON_CreateObject();
    NUM(p, "clockBestMonotonic", (double)start_ms);
    NUM(p, "clockBestWall", (double)(wall - (now - start_ms)));
    NUM(p, "clockMonotonic", (double)now);
    NUM(p, "clockStream", (double)now);
    NUM(p, "clockStreamRate", 1000);
    NUM(p, "clockWall", (double)wall);
    STR(p, "edgeType", edge);
    NUM(p, "eventId", event_id);
    STR(p, "eventType", "motion");
    cJSON *levels = cJSON_AddObjectToObject(p, "levels");
    NUM(levels, "0", event_level);
    int stop = !strcmp(edge, "stop");
    STR(p, "motionHeatmap", stop ? HEATMAP_NAME : "");
    NUM(p, "motionHeatmapWidth", stop ? MOTION_GW : 0);
    NUM(p, "motionHeatmapHeight", stop ? MOTION_GH : 0);
    pthread_mutex_lock(&lock);
    int have_snap = stop && snap;
    pthread_mutex_unlock(&lock);
    STR(p, "motionSnapshot", have_snap ? SNAPSHOT_NAME : "");
    NUM(p, "motionSnapshotWidth", have_snap ? snap_w : 0);
    NUM(p, "motionSnapshotHeight", have_snap ? snap_h : 0);
    STR(p, "motionSnapshotFullFoV", have_snap ? SNAPSHOT_NAME : "");
    NUM(p, "motionSnapshotFullFoVWidth", have_snap ? snap_w : 0);
    NUM(p, "motionSnapshotFullFoVHeight", have_snap ? snap_h : 0);
    return p;
}

static void on_start(const struct motion_report *r) {
    uint8_t *jpeg;
    size_t len;
    if (capture_jpeg(&jpeg, &len) == 0) {
        pthread_mutex_lock(&lock);
        free(snap);
        snap = jpeg;
        snap_len = len;
        pthread_mutex_unlock(&lock);
    }
    log_info("motion %u started (level %d, box %d,%d %dx%d)", event_id, r->level, r->box[0],
             r->box[1], r->box[2], r->box[3]);
    send_event("EventSmartMotion", event_payload("start", r->start_ms));
}

static void on_stop(const struct motion_report *r) {
    static uint8_t map[MOTION_GH][MOTION_GW];
    motion_heatmap(&motion, map);
    uint8_t *png;
    size_t len;
    if (png_gray(&map[0][0], MOTION_GW, MOTION_GH, &png, &len) == 0) {
        pthread_mutex_lock(&lock);
        free(heat);
        heat = png;
        heat_len = len;
        pthread_mutex_unlock(&lock);
    }
    log_info("motion %u stopped after %.1f s", event_id, (mono_ms() - r->start_ms) / 1000.0);
    send_event("EventSmartMotion", event_payload("stop", r->start_ms));
    event_id++;
}

static void *analytics_main(void *arg) {
    uint8_t *y = malloc((size_t)max_w * max_h);
    if (!y)
        return NULL;
    motion_init(&motion);
    int64_t next = mono_ms();
    for (;;) {
        int64_t wait = next - mono_ms();
        if (wait > 0)
            sleep_ms(wait);
        next += FRAME_MS;
        if (next < mono_ms())
            next = mono_ms() + FRAME_MS; // fell behind; don't try to catch up

        pthread_mutex_lock(&lock);
        if (settings_changed) {
            motion_configure(&motion, &settings);
            settings_changed = 0;
        }
        pthread_mutex_unlock(&lock);

        int w, h;
        if (capture_raw(y, (size_t)max_w * max_h, &w, &h, 0) < 0) {
            sleep_ms(1000);
            continue;
        }
        struct motion_report r;
        motion_feed(&motion, y, w, h, w, mono_ms(), &r);
        if (r.active)
            objects_motion(r.box, mono_ms());
        if (r.edge == MOTION_START)
            event_level = r.level;
        else if (motion.in_event && r.level > event_level)
            event_level = r.level;
        if (r.edge == MOTION_START)
            on_start(&r);
        else if (r.edge == MOTION_STOP)
            on_stop(&r);
    }
    return NULL;
}

void analytics_start(analytics_send_fn fn, int width, int height) {
    send_event = fn;
    max_w = width;
    max_h = height;
    // Distinct from the previous run's ids without keeping state.
    event_id = (uint32_t)(wall_ms() / 1000) & 0xffffff;
    pthread_t t;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    if (pthread_create(&t, &attr, analytics_main, NULL))
        log_error("analytics: cannot start thread");
    pthread_attr_destroy(&attr);
}

static int ms(const cJSON *o, const char *k) {
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k);
    return cJSON_IsNumber(v) ? (int)v->valuedouble : 0;
}

void analytics_settings(const cJSON *p) {
    struct motion_settings s = {0};
    s.start_ms = ms(p, "lingerEventStartMSec");
    s.stop_ms = ms(p, "lingerEventStopMSec");
    s.max_ms = ms(p, "eventMaxDurationMSec");
    const cJSON *z;
    cJSON_ArrayForEach(z, cJSON_GetObjectItemCaseSensitive(p, "zones")) {
        const cJSON *coord = cJSON_GetObjectItemCaseSensitive(z, "coord");
        int n = cJSON_GetArraySize(coord) / 2;
        if (s.nzones == MOTION_ZONES || n < 3)
            continue;
        struct motion_zone *mz = &s.zones[s.nzones++];
        mz->id = atoi(z->string);
        mz->level = cJSON_IsNumber(cJSON_GetObjectItemCaseSensitive(z, "level"))
                        ? ms(z, "level")
                        : 50;
        mz->npts = n > 16 ? 16 : n;
        for (int i = 0; i < mz->npts; i++) {
            mz->x[i] = (int)cJSON_GetArrayItem(coord, 2 * i)->valuedouble;
            mz->y[i] = (int)cJSON_GetArrayItem(coord, 2 * i + 1)->valuedouble;
        }
    }
    log_info("motion settings: %d zone(s), start %d ms, stop %d ms, enable=%d", s.nzones,
             s.start_ms, s.stop_ms, cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(p, "enable")));
    pthread_mutex_lock(&lock);
    settings = s;
    settings_changed = 1;
    pthread_mutex_unlock(&lock);
}

int analytics_file(const char *what, const char *filename, uint8_t **data, size_t *len,
                   const char **name) {
    if (objects_file(filename, data, len) == 0) {
        *name = filename;
        return 0;
    }
    int want_heat = (what && !strcmp(what, "motionHeatmap")) ||
                    (filename && !strcmp(filename, HEATMAP_NAME));
    int want_snap = (what && (!strcmp(what, "motionSnapshot") ||
                              !strcmp(what, "smartDetectZoneSnapshot"))) ||
                    (filename && !strcmp(filename, SNAPSHOT_NAME));
    if (!want_heat && !want_snap)
        return -1;
    int rc = -1;
    pthread_mutex_lock(&lock);
    const uint8_t *src = want_heat ? heat : snap;
    size_t n = want_heat ? heat_len : snap_len;
    if (src && (*data = malloc(n))) {
        memcpy(*data, src, n);
        *len = n;
        *name = want_heat ? HEATMAP_NAME : SNAPSHOT_NAME;
        rc = 0;
    }
    pthread_mutex_unlock(&lock);
    return rc;
}
