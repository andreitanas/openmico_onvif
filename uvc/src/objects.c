#include "objects.h"

#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <unistd.h>

#include "capture.h"
#include "detector.h"
#include "jpeg.h"
#include "smart.h"
#include "util.h"

// Detector input budget: Yolo-Fastest at 320x192 takes ~1.3 s on the T20.
#define INPUT_AREA (320 * 192)
// Keep detecting this long after the last motion.
#define LINGER_MS 3000
#define MAX_DETECTIONS 32
#define FILES 24 // snapshots kept for Protect to fetch

static analytics_send_fn send_event;
static struct detector *detector;
static int max_w, max_h;

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static int motion_box[4];       // guarded by lock: union since the last pass
static int have_motion;         // guarded by lock: motion_box has news
static int last_box[4];         // guarded by lock: the box of the last pass
static int64_t motion_until;    // guarded by lock
static struct smart_settings settings; // guarded by lock
static int settings_changed;           // guarded by lock

static pthread_mutex_t files_lock = PTHREAD_MUTEX_INITIALIZER;
static struct {
    char name[64];
    uint8_t *data;
    size_t len;
} files[FILES];
static int next_file;

static void keep_file(const char *name, uint8_t *data, size_t len) {
    pthread_mutex_lock(&files_lock);
    free(files[next_file].data);
    snprintf(files[next_file].name, sizeof(files[0].name), "%s", name);
    files[next_file].data = data;
    files[next_file].len = len;
    next_file = (next_file + 1) % FILES;
    pthread_mutex_unlock(&files_lock);
}

int objects_file(const char *filename, uint8_t **data, size_t *len) {
    int rc = -1;
    if (!filename || !*filename)
        return -1;
    pthread_mutex_lock(&files_lock);
    for (int i = 0; i < FILES; i++) {
        if (files[i].data && !strcmp(files[i].name, filename) && (*data = malloc(files[i].len))) {
            memcpy(*data, files[i].data, files[i].len);
            *len = files[i].len;
            rc = 0;
            break;
        }
    }
    pthread_mutex_unlock(&files_lock);
    return rc;
}

void objects_motion(const int *box, int64_t now) {
    pthread_mutex_lock(&lock);
    if (!have_motion) {
        memcpy(motion_box, box, sizeof(motion_box));
        have_motion = 1;
    } else {
        int x1 = motion_box[0] + motion_box[2], y1 = motion_box[1] + motion_box[3];
        int bx1 = box[0] + box[2], by1 = box[1] + box[3];
        motion_box[0] = box[0] < motion_box[0] ? box[0] : motion_box[0];
        motion_box[1] = box[1] < motion_box[1] ? box[1] : motion_box[1];
        motion_box[2] = (bx1 > x1 ? bx1 : x1) - motion_box[0];
        motion_box[3] = (by1 > y1 ? by1 : y1) - motion_box[1];
    }
    motion_until = now + LINGER_MS;
    pthread_mutex_unlock(&lock);
}

// NV12 (video range, BT.601) to packed RGB.
static void nv12_to_rgb(const uint8_t *nv12, int w, int h, uint8_t *rgb) {
    const uint8_t *uv = nv12 + w * h;
    for (int y = 0; y < h; y++) {
        const uint8_t *yr = nv12 + y * w, *uvr = uv + (y / 2) * w;
        uint8_t *o = rgb + y * w * 3;
        for (int x = 0; x < w; x++) {
            int c = 298 * (yr[x] - 16), d = uvr[x & ~1] - 128, e = uvr[x | 1] - 128;
            int r = (c + 409 * e + 128) >> 8, g = (c - 100 * d - 208 * e + 128) >> 8,
                b = (c + 516 * d + 128) >> 8;
            o[0] = r < 0 ? 0 : r > 255 ? 255 : r;
            o[1] = g < 0 ? 0 : g > 255 ? 255 : g;
            o[2] = b < 0 ? 0 : b > 255 ? 255 : b;
            o += 3;
        }
    }
}

// Bilinear resize of an RGB region (sx, sy, sw, sh of a w-wide image).
static void resize_rgb(const uint8_t *src, int w, int sx, int sy, int sw, int sh, uint8_t *dst,
                       int dw, int dh) {
    for (int y = 0; y < dh; y++) {
        int fy = ((2 * y + 1) * sh * 128 / (2 * dh)) - 64; // 1/128 px, pixel centres
        if (fy < 0)
            fy = 0;
        int y0 = fy >> 7, wy = fy & 127, y1 = y0 + 1 < sh ? y0 + 1 : y0;
        for (int x = 0; x < dw; x++) {
            int fx = ((2 * x + 1) * sw * 128 / (2 * dw)) - 64;
            if (fx < 0)
                fx = 0;
            int x0 = fx >> 7, wx = fx & 127, x1 = x0 + 1 < sw ? x0 + 1 : x0;
            const uint8_t *a = src + ((sy + y0) * w + sx + x0) * 3, *b = src + ((sy + y0) * w + sx + x1) * 3;
            const uint8_t *c = src + ((sy + y1) * w + sx + x0) * 3, *d = src + ((sy + y1) * w + sx + x1) * 3;
            uint8_t *o = dst + (y * dw + x) * 3;
            for (int k = 0; k < 3; k++) {
                int top = a[k] * (128 - wx) + b[k] * wx, bot = c[k] * (128 - wx) + d[k] * wx;
                o[k] = (top * (128 - wy) + bot * wy + 8192) >> 14;
            }
        }
    }
}

static int round32(int v) { return v < 32 ? 32 : (v + 16) / 32 * 32; }

// The part of the frame to run the detector on: the moving area with a
// margin, at least a quarter of the frame's height, or the whole frame
// when the motion covers most of it.
static void detect_region(const int *box, int w, int h, int *r) {
    int bx = box[0] * w / 1000, by = box[1] * h / 1000;
    int bw = box[2] * w / 1000, bh = box[3] * h / 1000;
    int mx = bw / 3 + 8, my = bh / 3 + 8;
    int x0 = bx - mx, y0 = by - my, x1 = bx + bw + mx, y1 = by + bh + my;
    int min = h / 4 < 96 ? 96 : h / 4;
    if (x1 - x0 < min) {
        x0 -= (min - (x1 - x0)) / 2;
        x1 = x0 + min;
    }
    if (y1 - y0 < min) {
        y0 -= (min - (y1 - y0)) / 2;
        y1 = y0 + min;
    }
    x0 = x0 < 0 ? 0 : x0;
    y0 = y0 < 0 ? 0 : y0;
    x1 = x1 > w ? w : x1;
    y1 = y1 > h ? h : y1;
    if ((x1 - x0) * (y1 - y0) * 2 > w * h) {
        x0 = y0 = 0;
        x1 = w;
        y1 = h;
    }
    r[0] = x0;
    r[1] = y0;
    r[2] = x1 - x0;
    r[3] = y1 - y0;
}

static void *objects_main(void *arg) {
    // Video and Protect come first: run at the lowest priority.
    setpriority(PRIO_PROCESS, (id_t)syscall(SYS_gettid), 19);
    size_t frame = (size_t)max_w * max_h;
    uint8_t *nv12 = malloc(frame * 3 / 2), *rgb = malloc(frame * 3), *crop = malloc(frame * 3);
    uint8_t *square = malloc(SMART_SNAPSHOT * SMART_SNAPSHOT * 3);
    if (!nv12 || !rgb || !crop || !square)
        return NULL;
    static struct smart smart;
    smart_init(&smart);
    struct detection dets[MAX_DETECTIONS];

    for (;;) {
        pthread_mutex_lock(&lock);
        if (settings_changed) {
            smart_configure(&smart, &settings);
            settings_changed = 0;
        }
        int active = mono_ms() < motion_until;
        if (have_motion) {
            memcpy(last_box, motion_box, sizeof(last_box));
            have_motion = 0;
        }
        int box[4];
        memcpy(box, last_box, sizeof(box));
        pthread_mutex_unlock(&lock);

        struct smart_result res;
        int w, h;
        if (!active || !smart_any_enabled(&smart) ||
            capture_raw(nv12, frame * 3 / 2, &w, &h, 1) < 0) {
            // Idle: let tracked objects time out and leave.
            smart_update(&smart, NULL, 0, mono_ms(), wall_ms(), max_w, max_h, &res);
            if (res.payload)
                send_event("EventSmartDetect", res.payload);
            sleep_ms(500);
            continue;
        }

        nv12_to_rgb(nv12, w, h, rgb);
        int reg[4];
        detect_region(box, w, h, reg);
        // Input size: the region's aspect at about INPUT_AREA pixels, never
        // more than 2x upscaled.
        double scale = sqrt((double)INPUT_AREA / (reg[2] * reg[3]));
        if (scale > 2)
            scale = 2;
        int in_w = round32((int)(reg[2] * scale)), in_h = round32((int)(reg[3] * scale));
        for (int y = 0; y < reg[3]; y++)
            memcpy(crop + y * reg[2] * 3, rgb + ((reg[1] + y) * w + reg[0]) * 3, reg[2] * 3);
        int64_t t0 = mono_ms();
        int n = detector_run(detector, crop, reg[2], reg[3], in_w, in_h, dets, MAX_DETECTIONS);
        int64_t took = mono_ms() - t0;
        // Back to whole-frame coordinates.
        for (int i = 0; i < n; i++) {
            dets[i].x = (reg[0] + dets[i].x * reg[2]) / w;
            dets[i].y = (reg[1] + dets[i].y * reg[3]) / h;
            dets[i].w = dets[i].w * reg[2] / w;
            dets[i].h = dets[i].h * reg[3] / h;
            log_debug("objects: %s %.2f at %.0f,%.0f %.0fx%.0f", detect_class_name(dets[i].cls),
                      dets[i].score, dets[i].x * w, dets[i].y * h, dets[i].w * w, dets[i].h * h);
        }
        log_debug("objects: %d found in %dx%d region at %dx%d, %lld ms", n, reg[2], reg[3], in_w,
                  in_h, (long long)took);

        smart_update(&smart, dets, n, mono_ms(), wall_ms(), w, h, &res);
        for (int i = 0; i < res.nsnap; i++) {
            const int *f = res.framing[i];
            int fx = f[0] * w / 1000, fy = f[1] * h / 1000, fw = f[2] * w / 1000, fh = f[3] * h / 1000;
            fw = fx + fw > w ? w - fx : fw;
            fh = fy + fh > h ? h - fy : fh;
            resize_rgb(rgb, w, fx, fy, fw, fh, square, SMART_SNAPSHOT, SMART_SNAPSHOT);
            uint8_t *jpg;
            size_t len;
            if (jpeg_rgb(square, SMART_SNAPSHOT, SMART_SNAPSHOT, 85, &jpg, &len) == 0)
                keep_file(res.snapshot_name[i], jpg, len);
        }
        if (res.want_fullfov) {
            uint8_t *jpg;
            size_t len;
            if (jpeg_rgb(rgb, w, h, 85, &jpg, &len) == 0)
                keep_file(res.fullfov_name, jpg, len);
        }
        if (res.payload) {
            cJSON *e = cJSON_GetObjectItem(res.payload, "edgeType");
            cJSON *types = cJSON_GetObjectItem(res.payload, "objectTypes");
            char *t = cJSON_PrintUnformatted(types);
            if (strcmp(e->valuestring, "moving"))
                log_info("objects: %s %s", e->valuestring, t ? t : "");
            free(t);
            send_event("EventSmartDetect", res.payload);
        }
    }
    return NULL;
}

int objects_start(analytics_send_fn fn, const char *model, int width, int height) {
    send_event = fn;
    max_w = width;
    max_h = height;
    if (!(detector = detector_open(model))) {
        log_warn("objects: cannot load %s.param/.bin; smart detections off", model);
        return -1;
    }
    pthread_t t;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    int rc = pthread_create(&t, &attr, objects_main, NULL);
    pthread_attr_destroy(&attr);
    if (rc) {
        log_error("objects: cannot start thread");
        return -1;
    }
    log_info("objects: detector %s loaded", model);
    return 0;
}

void objects_settings(const cJSON *p) {
    static const char *const names[DETECT_CLASSES] = {"person", "vehicle", "animal"};
    struct smart_settings s = {0};
    const cJSON *t;
    cJSON_ArrayForEach(t, cJSON_GetObjectItemCaseSensitive(p, "enableSmartDetect"))
        for (int c = 0; c < DETECT_CLASSES; c++)
            if (cJSON_IsString(t) && !strcmp(t->valuestring, names[c]))
                s.enabled[c] = 1;
    const cJSON *z;
    cJSON_ArrayForEach(z, cJSON_GetObjectItemCaseSensitive(p, "zones")) {
        const cJSON *coord = cJSON_GetObjectItemCaseSensitive(z, "coord");
        int n = cJSON_GetArraySize(coord) / 2;
        if (s.nzones == SMART_ZONES || n < 3)
            continue;
        int i = s.nzones++;
        struct motion_zone *mz = &s.zones[i];
        mz->id = atoi(z->string);
        const cJSON *sens = cJSON_GetObjectItemCaseSensitive(z, "sensitivity");
        mz->level = cJSON_IsNumber(sens) ? (int)sens->valuedouble : 50;
        mz->npts = n > 16 ? 16 : n;
        for (int k = 0; k < mz->npts; k++) {
            mz->x[k] = (int)cJSON_GetArrayItem(coord, 2 * k)->valuedouble;
            mz->y[k] = (int)cJSON_GetArrayItem(coord, 2 * k + 1)->valuedouble;
        }
        cJSON_ArrayForEach(t, cJSON_GetObjectItemCaseSensitive(z, "objectTypes"))
            for (int c = 0; c < DETECT_CLASSES; c++)
                if (cJSON_IsString(t) && !strcmp(t->valuestring, names[c]))
                    s.zone_types[i][c] = 1;
    }
    log_info("smart detect settings: person=%d vehicle=%d animal=%d, %d zone(s)", s.enabled[0],
             s.enabled[1], s.enabled[2], s.nzones);
    pthread_mutex_lock(&lock);
    settings = s;
    settings_changed = 1;
    pthread_mutex_unlock(&lock);
}
