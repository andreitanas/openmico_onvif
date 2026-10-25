#include "detector.h"

#include <stdio.h>
#include <stdlib.h>

#include "net.h"

struct detector {
    ncnn::Net net;
};

// COCO index (0-based) to Protect type, -1 for classes Protect has no use for.
static int coco_to_class(int coco) {
    switch (coco) {
    case 0:
        return DETECT_PERSON;
    case 1: // bicycle
    case 2: // car
    case 3: // motorcycle
    case 5: // bus
    case 7: // truck
        return DETECT_VEHICLE;
    case 14: // bird
    case 15: // cat
    case 16: // dog
    case 17: // horse
    case 18: // sheep
    case 19: // cow
    case 20: // elephant
    case 21: // bear
    case 22: // zebra
    case 23: // giraffe
        return DETECT_ANIMAL;
    default:
        return -1;
    }
}

struct detector *detector_open(const char *model) {
    detector *d = new detector;
    d->net.opt.num_threads = 1;
    d->net.opt.lightmode = true;
    char path[256];
    snprintf(path, sizeof(path), "%s.param", model);
    int rc = d->net.load_param(path);
    snprintf(path, sizeof(path), "%s.bin", model);
    if (rc != 0 || d->net.load_model(path) != 0) {
        delete d;
        return NULL;
    }
    return d;
}

int detector_run(struct detector *d, const uint8_t *rgb, int w, int h, int in_w, int in_h,
                 struct detection *out, int max) {
    ncnn::Mat in = ncnn::Mat::from_pixels_resize(rgb, ncnn::Mat::PIXEL_RGB, w, h, in_w, in_h);
    const float mean[3] = {0, 0, 0}, norm[3] = {1 / 255.f, 1 / 255.f, 1 / 255.f};
    in.substract_mean_normalize(mean, norm);
    ncnn::Extractor ex = d->net.create_extractor();
    ex.input("data", in);
    ncnn::Mat res;
    if (ex.extract("output", res) != 0)
        return 0;
    // Yolov3DetectionOutput rows: label (1-based, 0 = background), score,
    // x1, y1, x2, y2 (0..1).
    int n = 0;
    for (int i = 0; i < res.h && n < max; i++) {
        const float *v = res.row(i);
        int cls = coco_to_class((int)v[0] - 1);
        if (cls < 0)
            continue;
        struct detection *o = &out[n++];
        o->cls = (enum detect_class)cls;
        o->score = v[1];
        o->x = v[2];
        o->y = v[3];
        o->w = v[4] - v[2];
        o->h = v[5] - v[3];
    }
    return n;
}

void detector_close(struct detector *d) { delete d; }
