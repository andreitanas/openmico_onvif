// Times one ncnn detector on a raw RGB image and prints its detections.
//   bench model.param model.bin image.rgb W H inW inH iterations [threshold]
// Handles Yolo-Fastest (built-in Yolov3DetectionOutput layer) and FastestDet
// (raw grid output, decoded here).

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <algorithm>
#include <vector>

#include "benchmark.h"
#include "net.h"

static const char *coco[] = {
    "person", "bicycle", "car", "motorcycle", "airplane", "bus", "train", "truck", "boat",
    "traffic light", "fire hydrant", "stop sign", "parking meter", "bench", "bird", "cat", "dog",
    "horse", "sheep", "cow", "elephant", "bear", "zebra", "giraffe", "backpack", "umbrella",
    "handbag", "tie", "suitcase", "frisbee", "skis", "snowboard", "sports ball", "kite",
    "baseball bat", "baseball glove", "skateboard", "surfboard", "tennis racket", "bottle",
    "wine glass", "cup", "fork", "knife", "spoon", "bowl", "banana", "apple", "sandwich", "orange",
    "broccoli", "carrot", "hot dog", "pizza", "donut", "cake", "chair", "couch", "potted plant",
    "bed", "dining table", "toilet", "tv", "laptop", "mouse", "remote", "keyboard", "cell phone",
    "microwave", "oven", "toaster", "sink", "refrigerator", "book", "clock", "vase", "scissors",
    "teddy bear", "hair drier", "toothbrush"};

struct box {
    int label;
    float score, x1, y1, x2, y2; // normalized 0..1
};

static long peak_rss_kb() {
    FILE *fp = fopen("/proc/self/status", "r");
    if (!fp)
        return -1;
    char line[128];
    long kb = -1;
    while (fgets(line, sizeof(line), fp))
        if (!strncmp(line, "VmHWM:", 6))
            kb = atol(line + 6);
    fclose(fp);
    return kb;
}

static float iou(const box &a, const box &b) {
    float iw = std::min(a.x2, b.x2) - std::max(a.x1, b.x1);
    float ih = std::min(a.y2, b.y2) - std::max(a.y1, b.y1);
    if (iw <= 0 || ih <= 0)
        return 0;
    float inter = iw * ih;
    return inter / ((a.x2 - a.x1) * (a.y2 - a.y1) + (b.x2 - b.x1) * (b.y2 - b.y1) - inter);
}

// FastestDet: one cell per stride-16 position; channel 0 objectness,
// 1-2 center offsets (tanh), 3-4 size (sigmoid), 5.. class scores.
static std::vector<box> decode_fastestdet(const ncnn::Mat &out, float thresh) {
    std::vector<box> cand, kept;
    int classes = out.c - 5;
    for (int y = 0; y < out.h; y++) {
        for (int x = 0; x < out.w; x++) {
            float obj = out.channel(0).row(y)[x];
            int label = 0;
            float best = 0;
            for (int c = 0; c < classes; c++) {
                float s = out.channel(5 + c).row(y)[x];
                if (s > best) {
                    best = s;
                    label = c;
                }
            }
            float score = powf(best, 0.4f) * powf(obj, 0.6f);
            if (score <= thresh)
                continue;
            float cx = (x + tanhf(out.channel(1).row(y)[x])) / out.w;
            float cy = (y + tanhf(out.channel(2).row(y)[x])) / out.h;
            float bw = 1 / (1 + expf(-out.channel(3).row(y)[x]));
            float bh = 1 / (1 + expf(-out.channel(4).row(y)[x]));
            cand.push_back({label, score, cx - bw / 2, cy - bh / 2, cx + bw / 2, cy + bh / 2});
        }
    }
    std::sort(cand.begin(), cand.end(), [](const box &a, const box &b) { return a.score > b.score; });
    for (const box &b : cand) {
        bool keep = true;
        for (const box &k : kept)
            if (k.label == b.label && iou(k, b) > 0.45f)
                keep = false;
        if (keep)
            kept.push_back(b);
    }
    return kept;
}

int main(int argc, char **argv) {
    if (argc < 9) {
        fprintf(stderr, "usage: %s model.param model.bin image.rgb W H inW inH iterations [threshold]\n",
                argv[0]);
        return 2;
    }
    int w = atoi(argv[4]), h = atoi(argv[5]), iw = atoi(argv[6]), ih = atoi(argv[7]);
    int iters = atoi(argv[8]);
    float thresh = argc > 9 ? atof(argv[9]) : 0.5f;

    FILE *fp = fopen(argv[3], "rb");
    if (!fp)
        return 1;
    unsigned char *rgb = (unsigned char *)malloc(w * h * 3);
    size_t got = fread(rgb, 1, w * h * 3, fp);
    fclose(fp);
    if (got != (size_t)(w * h * 3)) {
        fprintf(stderr, "short image\n");
        return 1;
    }

    // The two model families differ in blob names and output decoding.
    bool fastestdet = false;
    fp = fopen(argv[1], "r");
    if (fp) {
        char line[512];
        fastestdet = true;
        while (fgets(line, sizeof(line), fp))
            if (strstr(line, "Yolov3DetectionOutput"))
                fastestdet = false;
        fclose(fp);
    }
    const char *in_name = fastestdet ? "input.1" : "data";
    const char *out_name = fastestdet ? "758" : "output";

    ncnn::Net net;
    net.opt.num_threads = 1;
    net.opt.lightmode = true;
    if (net.load_param(argv[1]) || net.load_model(argv[2])) {
        fprintf(stderr, "cannot load model\n");
        return 1;
    }

    double total = 0, best = 1e9, worst = 0;
    for (int it = 0; it < iters; it++) {
        double t0 = ncnn::get_current_time();
        ncnn::Mat in = ncnn::Mat::from_pixels_resize(rgb, ncnn::Mat::PIXEL_RGB, w, h, iw, ih);
        const float mean[3] = {0, 0, 0}, norm[3] = {1 / 255.f, 1 / 255.f, 1 / 255.f};
        in.substract_mean_normalize(mean, norm);
        ncnn::Extractor ex = net.create_extractor();
        ex.input(in_name, in);
        ncnn::Mat out;
        ex.extract(out_name, out);
        std::vector<box> boxes;
        if (fastestdet) {
            boxes = decode_fastestdet(out, thresh);
        } else {
            for (int i = 0; i < out.h; i++) {
                const float *v = out.row(i);
                if (v[1] >= thresh) // 0 is background in ncnn's YOLO output
                    boxes.push_back({(int)v[0] - 1, v[1], v[2], v[3], v[4], v[5]});
            }
        }
        double dt = ncnn::get_current_time() - t0;
        if (it == 0) {
            for (const box &b : boxes)
                printf("  %-12s %.2f  box %4.0f,%4.0f - %4.0f,%4.0f\n",
                       b.label >= 0 && b.label < 80 ? coco[b.label] : "?", b.score, b.x1 * w,
                       b.y1 * h, b.x2 * w, b.y2 * h);
        } else {
            // First run includes one-time allocations; time the rest.
            total += dt;
            best = dt < best ? dt : best;
            worst = dt > worst ? dt : worst;
        }
    }
    if (iters > 1)
        printf("%dx%d: mean %.0f ms, min %.0f, max %.0f over %d runs; peak RSS %ld KB\n", iw, ih,
               total / (iters - 1), best, worst, iters - 1, peak_rss_kb());
    free(rgb);
    return 0;
}
