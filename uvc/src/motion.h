#ifndef UVC_MOTION_H
#define UVC_MOTION_H

#include <stdint.h>

// Motion detection on the luma plane of low-resolution frames, against a
// slowly adapting background, on a grid of cells. No I/O: fed frames and
// times by the caller, it reports when motion events start and stop.

#define MOTION_GW 160
#define MOTION_GH 90
#define MOTION_ZONES 8

// Protect's zones: a polygon in 0-1000 coordinates and a sensitivity level
// (0-100, higher triggers on smaller changes).
struct motion_zone {
    int id;
    int npts;
    int x[16], y[16];
    int level;
};

struct motion_settings {
    int start_ms; // motion must last this long before an event starts
    int stop_ms;  // and be gone this long before it stops
    int max_ms;   // longest event; a new one starts if motion continues
    int nzones;   // 0: the whole picture at level 50
    struct motion_zone zones[MOTION_ZONES];
};

enum motion_edge { MOTION_NONE, MOTION_START, MOTION_STOP };

struct motion_report {
    enum motion_edge edge;
    int active;          // motion in this frame (any zone)
    int level;           // 0-100: share of the triggering zone that moved, scaled
    int box[4];          // x, y, w, h (0-1000) around this frame's motion
    int64_t start_ms;    // first motion of the current/last event
};

struct motion {
    struct motion_settings set;
    uint8_t zone_mask[MOTION_GH][MOTION_GW]; // bit i: cell in zone i
    int zone_cells[MOTION_ZONES];
    int zone_need[MOTION_ZONES];             // moving cells needed to trigger
    uint16_t bg[MOTION_GH][MOTION_GW];       // background, luma << 4
    uint8_t cur[MOTION_GH][MOTION_GW];
    uint8_t diff[MOTION_GH][MOTION_GW];      // differs from the background
    uint8_t act[MOTION_GH][MOTION_GW];       // moving, after the speckle filter
    uint16_t heat[MOTION_GH][MOTION_GW];     // moving-frame count per cell this event
    int noise;                               // mean |cur - bg| of still cells, << 4
    int have_bg;
    int64_t hold_until;                      // ignore frames after a lighting change
    int in_event;
    int64_t first_motion, last_motion, event_start;
};

void motion_init(struct motion *m);
// Apply Protect's settings; keeps the background.
void motion_configure(struct motion *m, const struct motion_settings *s);
// Feed one frame (luma, w x h with the given stride), taken at now_ms.
void motion_feed(struct motion *m, const uint8_t *y, int w, int h, int stride, int64_t now_ms,
                 struct motion_report *r);
// Where motion happened during the last event, scaled to 0-255 per cell.
void motion_heatmap(const struct motion *m, uint8_t out[MOTION_GH][MOTION_GW]);

#endif
