#include "motion.h"

#include <string.h>

// A cell moves when it differs from the background by more than this many
// luma steps, or by 3x the picture's current noise if that is larger (IR
// night images are much noisier).
#define MIN_DIFF 10
// A frame where more than this share of cells changed is a lighting change
// (IR cut filter, lights switched on), not motion: the background restarts.
#define GLOBAL_PERCENT 45
#define GLOBAL_HOLD_MS 2000
// Gaps shorter than this keep motion "continuous" while an event starts.
#define START_GAP_MS 600

static int inside(const struct motion_zone *z, int px, int py) {
    int in = 0;
    for (int i = 0, j = z->npts - 1; i < z->npts; j = i++) {
        if ((z->y[i] > py) != (z->y[j] > py) &&
            px < (z->x[j] - z->x[i]) * (py - z->y[i]) / (z->y[j] - z->y[i]) + z->x[i])
            in = !in;
    }
    return in;
}

// Moving cells a zone needs, from its sensitivity: level 100 triggers on
// 0.1% of the zone, level 50 on ~0.7%, level 0 on 5%.
static int cells_needed(int cells, int level) {
    static const int permille[11] = {50, 34, 23, 15, 10, 7, 5, 3, 2, 1, 1};
    int l = level < 0 ? 0 : level > 100 ? 100 : level;
    int n = cells * permille[(l + 5) / 10] / 1000;
    return n < 2 ? 2 : n;
}

void motion_configure(struct motion *m, const struct motion_settings *s) {
    m->set = *s;
    if (m->set.nzones <= 0) {
        struct motion_zone *z = &m->set.zones[0];
        *z = (struct motion_zone){.id = 1, .npts = 4, .level = 50};
        int xs[4] = {0, 1000, 1000, 0}, ys[4] = {0, 0, 1000, 1000};
        memcpy(z->x, xs, sizeof(xs));
        memcpy(z->y, ys, sizeof(ys));
        m->set.nzones = 1;
    }
    if (m->set.nzones > MOTION_ZONES)
        m->set.nzones = MOTION_ZONES;
    if (m->set.start_ms <= 0)
        m->set.start_ms = 1000;
    if (m->set.stop_ms <= 0)
        m->set.stop_ms = 3000;
    if (m->set.max_ms <= 0)
        m->set.max_ms = 300000;
    memset(m->zone_mask, 0, sizeof(m->zone_mask));
    memset(m->zone_cells, 0, sizeof(m->zone_cells));
    for (int gy = 0; gy < MOTION_GH; gy++) {
        for (int gx = 0; gx < MOTION_GW; gx++) {
            int px = (2 * gx + 1) * 1000 / (2 * MOTION_GW), py = (2 * gy + 1) * 1000 / (2 * MOTION_GH);
            for (int i = 0; i < m->set.nzones; i++) {
                if (inside(&m->set.zones[i], px, py)) {
                    m->zone_mask[gy][gx] |= 1 << i;
                    m->zone_cells[i]++;
                }
            }
        }
    }
    for (int i = 0; i < m->set.nzones; i++)
        m->zone_need[i] = cells_needed(m->zone_cells[i], m->set.zones[i].level);
}

void motion_init(struct motion *m) {
    memset(m, 0, sizeof(*m));
    struct motion_settings s = {0};
    motion_configure(m, &s);
}

// Mean luma of each cell.
static void downscale(struct motion *m, const uint8_t *y, int w, int h, int stride) {
    int cw = w / MOTION_GW, ch = h / MOTION_GH;
    int area = cw * ch;
    for (int gy = 0; gy < MOTION_GH; gy++) {
        const uint8_t *row = y + gy * ch * stride;
        for (int gx = 0; gx < MOTION_GW; gx++) {
            const uint8_t *p = row + gx * cw;
            int sum = 0;
            for (int yy = 0; yy < ch; yy++, p += stride)
                for (int xx = 0; xx < cw; xx++)
                    sum += p[xx];
            m->cur[gy][gx] = sum / area;
        }
    }
}

static void reset_background(struct motion *m) {
    for (int gy = 0; gy < MOTION_GH; gy++)
        for (int gx = 0; gx < MOTION_GW; gx++)
            m->bg[gy][gx] = m->cur[gy][gx] << 4;
    m->have_bg = 1;
}

void motion_feed(struct motion *m, const uint8_t *y, int w, int h, int stride, int64_t now_ms,
                 struct motion_report *r) {
    memset(r, 0, sizeof(*r));
    downscale(m, y, w, h, stride);
    if (!m->have_bg) {
        reset_background(m);
        m->noise = 2 << 4;
        return;
    }

    // Cells that differ from the background.
    int thr = 3 * m->noise >> 4;
    if (thr < MIN_DIFF)
        thr = MIN_DIFF;
    int raw = 0;
    long still_sum = 0, still_n = 0;
    for (int gy = 0; gy < MOTION_GH; gy++) {
        for (int gx = 0; gx < MOTION_GW; gx++) {
            int d = (m->cur[gy][gx] << 4) - m->bg[gy][gx];
            d = (d < 0 ? -d : d) >> 4;
            m->diff[gy][gx] = d > thr;
            raw += m->diff[gy][gx];
            if (!m->diff[gy][gx]) {
                still_sum += d;
                still_n++;
            }
        }
    }
    if (still_n)
        m->noise += ((int)(still_sum * 16 / still_n) - m->noise) / 16;

    if (raw * 100 > GLOBAL_PERCENT * MOTION_GW * MOTION_GH) {
        reset_background(m);
        m->hold_until = now_ms + GLOBAL_HOLD_MS;
    }
    int hold = now_ms < m->hold_until;

    // Speckle filter: a cell moves if at least 4 of the 3x3 around it differ.
    int x0 = MOTION_GW, y0 = MOTION_GH, x1 = -1, y1 = -1;
    int zone_moving[MOTION_ZONES] = {0};
    for (int gy = 0; gy < MOTION_GH; gy++) {
        for (int gx = 0; gx < MOTION_GW; gx++) {
            int n = 0;
            for (int dy = -1; dy <= 1; dy++)
                for (int dx = -1; dx <= 1; dx++) {
                    int yy = gy + dy, xx = gx + dx;
                    if (yy >= 0 && yy < MOTION_GH && xx >= 0 && xx < MOTION_GW)
                        n += m->diff[yy][xx];
                }
            int a = !hold && m->diff[gy][gx] && n >= 4 && m->zone_mask[gy][gx];
            m->act[gy][gx] = a;
            if (!a)
                continue;
            for (int i = 0; i < m->set.nzones; i++)
                if (m->zone_mask[gy][gx] & (1 << i))
                    zone_moving[i]++;
            if (gx < x0) x0 = gx;
            if (gx > x1) x1 = gx;
            if (gy < y0) y0 = gy;
            if (gy > y1) y1 = gy;
        }
    }

    // Background follows the picture: quickly where it is still, slowly
    // where something moves, so objects that stop become background.
    for (int gy = 0; gy < MOTION_GH; gy++) {
        for (int gx = 0; gx < MOTION_GW; gx++) {
            int c = m->cur[gy][gx] << 4, b = m->bg[gy][gx];
            m->bg[gy][gx] = b + (c - b) / (m->act[gy][gx] ? 128 : 16);
        }
    }

    for (int i = 0; i < m->set.nzones; i++) {
        if (zone_moving[i] >= m->zone_need[i]) {
            r->active = 1;
            int level = zone_moving[i] * 100 * 10 / (m->zone_cells[i] ? m->zone_cells[i] : 1);
            if (level > 100)
                level = 100;
            if (level > r->level)
                r->level = level;
        }
    }
    if (r->active) {
        r->box[0] = x0 * 1000 / MOTION_GW;
        r->box[1] = y0 * 1000 / MOTION_GH;
        r->box[2] = (x1 + 1 - x0) * 1000 / MOTION_GW;
        r->box[3] = (y1 + 1 - y0) * 1000 / MOTION_GH;
    }

    // Event state: start after start_ms of (nearly) continuous motion,
    // stop after stop_ms without any, or at max_ms.
    if (r->active) {
        if (!m->first_motion || now_ms - m->last_motion > START_GAP_MS)
            m->first_motion = now_ms;
        m->last_motion = now_ms;
    }
    if (!m->in_event) {
        if (r->active && now_ms - m->first_motion >= m->set.start_ms) {
            m->in_event = 1;
            m->event_start = m->first_motion;
            memset(m->heat, 0, sizeof(m->heat));
            r->edge = MOTION_START;
        }
    } else if (now_ms - m->last_motion >= m->set.stop_ms ||
               now_ms - m->event_start >= m->set.max_ms) {
        m->in_event = 0;
        m->first_motion = 0;
        r->edge = MOTION_STOP;
    }
    if (m->in_event || r->edge == MOTION_STOP) {
        for (int gy = 0; gy < MOTION_GH; gy++)
            for (int gx = 0; gx < MOTION_GW; gx++)
                if (m->act[gy][gx] && m->heat[gy][gx] < 0xffff)
                    m->heat[gy][gx]++;
    }
    r->start_ms = m->in_event || r->edge == MOTION_STOP ? m->event_start : m->first_motion;
}

void motion_heatmap(const struct motion *m, uint8_t out[MOTION_GH][MOTION_GW]) {
    int max = 1;
    for (int gy = 0; gy < MOTION_GH; gy++)
        for (int gx = 0; gx < MOTION_GW; gx++)
            if (m->heat[gy][gx] > max)
                max = m->heat[gy][gx];
    for (int gy = 0; gy < MOTION_GH; gy++)
        for (int gx = 0; gx < MOTION_GW; gx++)
            out[gy][gx] = m->heat[gy][gx] * 255 / max;
}
