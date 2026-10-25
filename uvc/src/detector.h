#ifndef UVC_DETECTOR_H
#define UVC_DETECTOR_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Object detector (Yolo-Fastest via ncnn) with COCO classes folded into
// Protect's smart-detect types.
enum detect_class { DETECT_PERSON, DETECT_VEHICLE, DETECT_ANIMAL, DETECT_CLASSES };

struct detection {
    enum detect_class cls;
    float score;
    float x, y, w, h; // 0..1 of the image passed in
};

struct detector;

// Loads <model>.param and <model>.bin. NULL on failure.
struct detector *detector_open(const char *model);
// Detect in an RGB image (w x h, packed) scaled to in_w x in_h (multiples
// of 32). Returns the number of detections written to out.
int detector_run(struct detector *d, const uint8_t *rgb, int w, int h, int in_w, int in_h,
                 struct detection *out, int max);
void detector_close(struct detector *d);

// Protect's name for the type.
static inline const char *detect_class_name(enum detect_class c) {
    static const char *const names[] = {"person", "vehicle", "animal"};
    return c >= 0 && c < DETECT_CLASSES ? names[c] : "?";
}

#ifdef __cplusplus
}
#endif

#endif
