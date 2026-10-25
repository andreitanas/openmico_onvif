#ifndef UVC_SMART_H
#define UVC_SMART_H

#include <stdint.h>

#include "cJSON.h"
#include "detector.h"
#include "motion.h"

// Smart detections for Protect: tracks detector boxes over time, confirms
// objects, and produces EventSmartDetect payloads (enter, moving, leave) in
// the shape real Gen4/Gen5 cameras send. No I/O; the caller runs the
// detector, encodes the snapshots it asks for and sends the payloads.

#define SMART_TRACKS 16
#define SMART_ZONES 8
#define SMART_SNAPSHOT 360 // object crops are this size, square

struct smart_settings {
    int enabled[DETECT_CLASSES];
    int nzones; // 0: the whole picture, sensitivity 50, all types
    struct motion_zone zones[SMART_ZONES]; // level = sensitivity
    int zone_types[SMART_ZONES][DETECT_CLASSES];
};

struct smart_track {
    uint32_t id;
    enum detect_class cls;
    int box[4];       // x, y, w, h in 0-1000
    int score;        // 0-100, latest
    int hits;
    int confirmed;
    int zones;        // bit i: in settings zone i
    int64_t first_wall, last_mono;
    // Best sighting, for the leave frame's per-object snapshot.
    int best_score;
    int best_box[4], framing[4];
    int64_t best_mono, best_wall;
    char snapshot[64];
};

// What the caller should do after an update.
struct smart_result {
    cJSON *payload; // EventSmartDetect to send, or NULL
    // Detections (indexes into the update's array) that are now their
    // track's best sighting: encode a SMART_SNAPSHOT square crop of
    // framing[] (0-1000) as snapshot_name[] and keep it for Protect.
    int nsnap;
    int snap_det[SMART_TRACKS];
    int framing[SMART_TRACKS][4];
    char snapshot_name[SMART_TRACKS][64];
    // Keep this pass's whole frame as the event's full-FoV snapshot.
    int want_fullfov;
    char fullfov_name[64];
};

struct smart {
    struct smart_settings set;
    struct smart_track tracks[SMART_TRACKS];
    uint32_t next_tracker, next_event;
    int in_event, fullfov_score, fullfov_seq;
    char fullfov[64];
    int fullfov_w, fullfov_h;
};

void smart_init(struct smart *s);
void smart_configure(struct smart *s, const struct smart_settings *set);
int smart_any_enabled(const struct smart *s);
// One detector pass (dets in 0..1 of the full frame), or a tick with n = 0
// when the detector is idle, so objects can leave. Full-FoV snapshots are
// fov_w x fov_h.
void smart_update(struct smart *s, const struct detection *dets, int n, int64_t mono_ms,
                  int64_t wall_ms, int fov_w, int fov_h, struct smart_result *r);

#endif
