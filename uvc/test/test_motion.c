// Motion detector on synthetic 640x360 frames at 5 fps: sensor noise alone,
// a moving square, a lighting change, a square outside the zone, and a
// small object at high sensitivity.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "motion.h"

#define W 640
#define H 360

static uint8_t frame[W * H];
static unsigned rng = 1;
static int failures;

static int noise(int amp) {
    rng = rng * 1103515245 + 12345;
    return (int)((rng >> 16) % (2 * amp + 1)) - amp;
}

// Gradient background with noise, an optional bright square, overall gain.
static void render(int sq_x, int sq_y, int sq, int gain) {
    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++) {
            int v = 60 + x / 8 + y / 6 + noise(6);
            if (sq && x >= sq_x && x < sq_x + sq && y >= sq_y && y < sq_y + sq)
                v = 220 + noise(6);
            v = v * gain / 100;
            frame[y * W + x] = v < 0 ? 0 : v > 255 ? 255 : v;
        }
    }
}

struct run {
    int starts, stops, active_frames;
    int64_t first_start;
};

static void feed(struct motion *m, int64_t *t, int frames, struct run *r, int sq_x0, int sq_y,
                 int sq, int dx, int gain) {
    for (int i = 0; i < frames; i++, *t += 200) {
        render(sq_x0 + i * dx, sq_y, sq, gain);
        struct motion_report rep;
        motion_feed(m, frame, W, H, W, *t, &rep);
        r->active_frames += rep.active;
        if (rep.edge == MOTION_START && !r->starts++)
            r->first_start = *t;
        if (rep.edge == MOTION_STOP)
            r->stops++;
    }
}

static void expect(const char *what, int ok) {
    printf("%-58s %s\n", what, ok ? "ok" : "FAILED");
    failures += !ok;
}

int main(void) {
    static struct motion m;
    int64_t t = 1000;
    motion_init(&m);

    struct run r = {0};
    feed(&m, &t, 50, &r, 0, 0, 0, 0, 100);
    expect("10 s of sensor noise: no motion", r.starts == 0 && r.active_frames == 0);

    memset(&r, 0, sizeof(r));
    int64_t t0 = t;
    feed(&m, &t, 15, &r, 40, 150, 48, 24, 100);
    expect("square moving for 3 s: one event start", r.starts == 1);
    expect("  started after the 1 s linger", r.first_start - t0 >= 1000 && r.first_start - t0 <= 1600);
    feed(&m, &t, 25, &r, 0, 0, 0, 0, 100);
    expect("  stops once the picture is still for 3 s", r.stops == 1);

    uint8_t heat[MOTION_GH][MOTION_GW];
    motion_heatmap(&m, heat);
    int hot_row = 150 * MOTION_GH / H + 2, hot = 0, cold = 0;
    for (int gx = 0; gx < MOTION_GW; gx++)
        hot += heat[hot_row][gx] > 0;
    for (int gx = 0; gx < MOTION_GW; gx++)
        cold += heat[5][gx] > 0;
    expect("  heatmap covers the square's path only", hot > 20 && cold == 0);

    memset(&r, 0, sizeof(r));
    feed(&m, &t, 1, &r, 0, 0, 0, 0, 160);
    feed(&m, &t, 30, &r, 0, 0, 0, 0, 160);
    expect("lights brighten the whole picture: no event", r.starts == 0);

    // Zone: left half only; the square moves in the right half.
    struct motion_settings s = {.start_ms = 1000, .stop_ms = 3000, .max_ms = 300000, .nzones = 1};
    s.zones[0] = (struct motion_zone){.id = 1, .npts = 4, .level = 50};
    int xs[4] = {0, 450, 450, 0}, ys[4] = {0, 0, 1000, 1000};
    memcpy(s.zones[0].x, xs, sizeof(xs));
    memcpy(s.zones[0].y, ys, sizeof(ys));
    motion_configure(&m, &s);
    memset(&r, 0, sizeof(r));
    feed(&m, &t, 15, &r, 380, 150, 48, 12, 160);
    expect("square moving outside the zone: no event", r.starts == 0);
    feed(&m, &t, 25, &r, 0, 0, 0, 0, 160);

    // A small object (12x12 px) only triggers at high sensitivity.
    s.zones[0].level = 20;
    motion_configure(&m, &s);
    memset(&r, 0, sizeof(r));
    feed(&m, &t, 15, &r, 40, 100, 12, 8, 160);
    expect("small object at level 20: ignored", r.starts == 0);
    feed(&m, &t, 25, &r, 0, 0, 0, 0, 160);
    s.zones[0].level = 100;
    motion_configure(&m, &s);
    memset(&r, 0, sizeof(r));
    feed(&m, &t, 15, &r, 40, 100, 12, 8, 160);
    expect("small object at level 100: detected", r.starts == 1);

    return failures ? 1 : 0;
}
