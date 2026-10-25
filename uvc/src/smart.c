#include "smart.h"

#include <stdio.h>
#include <string.h>

// A track not seen for this long is gone. The detector takes ~1-1.5 s per
// pass on the T20 and misses objects now and then.
#define LOST_MS 4500
// Confirmed after this many sightings, or one at this score.
#define CONFIRM_HITS 2
#define CONFIRM_SCORE 65
// Real cameras: 275 ms at ~4 updates/s; ours update about once a second.
#define DISPLAY_TIMEOUT_MS 1500

#define NUM(o, k, v) cJSON_AddNumberToObject(o, k, v)
#define STR(o, k, v) cJSON_AddStringToObject(o, k, v)

void smart_configure(struct smart *s, const struct smart_settings *set) {
    s->set = *set;
    if (s->set.nzones <= 0) {
        struct motion_zone *z = &s->set.zones[0];
        *z = (struct motion_zone){.id = 1, .npts = 4, .level = 50};
        int xs[4] = {0, 1000, 1000, 0}, ys[4] = {0, 0, 1000, 1000};
        memcpy(z->x, xs, sizeof(xs));
        memcpy(z->y, ys, sizeof(ys));
        for (int c = 0; c < DETECT_CLASSES; c++)
            s->set.zone_types[0][c] = 1;
        s->set.nzones = 1;
    }
}

void smart_init(struct smart *s) {
    memset(s, 0, sizeof(*s));
    s->next_tracker = 1;
    s->next_event = 1;
    struct smart_settings set = {.enabled = {1, 1, 1}};
    smart_configure(s, &set);
}

int smart_any_enabled(const struct smart *s) {
    for (int c = 0; c < DETECT_CLASSES; c++)
        if (s->set.enabled[c])
            return 1;
    return 0;
}

static int inside(const struct motion_zone *z, int px, int py) {
    int in = 0;
    for (int i = 0, j = z->npts - 1; i < z->npts; j = i++) {
        if ((z->y[i] > py) != (z->y[j] > py) &&
            px < (z->x[j] - z->x[i]) * (py - z->y[i]) / (z->y[j] - z->y[i]) + z->x[i])
            in = !in;
    }
    return in;
}

// Score (0-100) a zone needs, from its sensitivity: 50 -> 45, 100 -> 25.
static int zone_threshold(const struct motion_zone *z) {
    return 65 - 40 * z->level / 100;
}

// Zones (bitmask) an object counts in: its centre inside the polygon, its
// type allowed there, and its score above the zone's threshold.
static int object_zones(const struct smart *s, enum detect_class cls, const int *box, int score) {
    int cx = box[0] + box[2] / 2, cy = box[1] + box[3] / 2, mask = 0;
    for (int i = 0; i < s->set.nzones; i++) {
        const struct motion_zone *z = &s->set.zones[i];
        if (s->set.zone_types[i][cls] && score >= zone_threshold(z) && inside(z, cx, cy))
            mask |= 1 << i;
    }
    return mask;
}

static int iou_permille(const int *a, const int *b) {
    int x0 = a[0] > b[0] ? a[0] : b[0], y0 = a[1] > b[1] ? a[1] : b[1];
    int x1 = a[0] + a[2] < b[0] + b[2] ? a[0] + a[2] : b[0] + b[2];
    int y1 = a[1] + a[3] < b[1] + b[3] ? a[1] + a[3] : b[1] + b[3];
    if (x1 <= x0 || y1 <= y0)
        return 0;
    long inter = (long)(x1 - x0) * (y1 - y0);
    long uni = (long)a[2] * a[3] + (long)b[2] * b[3] - inter;
    return uni > 0 ? (int)(inter * 1000 / uni) : 0;
}

// Same object if the boxes overlap or the centres are within a box size.
static int match_cost(const struct smart_track *t, const int *box) {
    int iou = iou_permille(t->box, box);
    if (iou >= 100)
        return 1000 - iou;
    int dx = (t->box[0] + t->box[2] / 2) - (box[0] + box[2] / 2);
    int dy = (t->box[1] + t->box[3] / 2) - (box[1] + box[3] / 2);
    int reach = t->box[2] > t->box[3] ? t->box[2] : t->box[3];
    if (dx * dx + dy * dy > reach * reach)
        return -1;
    return 1000 + (dx < 0 ? -dx : dx) + (dy < 0 ? -dy : dy);
}

// Square crop around the object, in frame pixels, as 0-1000 coordinates.
static void framing_rect(const int *box, int fov_w, int fov_h, int *out) {
    int bx = box[0] * fov_w / 1000, by = box[1] * fov_h / 1000;
    int bw = box[2] * fov_w / 1000, bh = box[3] * fov_h / 1000;
    int side = (bw > bh ? bw : bh) * 5 / 4;
    if (side < 48)
        side = 48;
    if (side > fov_h)
        side = fov_h;
    if (side > fov_w)
        side = fov_w;
    int x = bx + bw / 2 - side / 2, y = by + bh / 2 - side / 2;
    x = x < 0 ? 0 : x + side > fov_w ? fov_w - side : x;
    y = y < 0 ? 0 : y + side > fov_h ? fov_h - side : y;
    out[0] = x * 1000 / fov_w;
    out[1] = y * 1000 / fov_h;
    out[2] = side * 1000 / fov_w;
    out[3] = side * 1000 / fov_h;
}

static cJSON *int_array(const int *v, int n) { return cJSON_CreateIntArray(v, n); }

static cJSON *descriptor(const struct smart *s, const struct smart_track *t) {
    static const char *const colors[] = {"red", "cyan", "white"};
    cJSON *d = cJSON_CreateObject();
    cJSON_AddNullToObject(d, "attributes");
    STR(d, "boxColor", colors[t->cls]);
    NUM(d, "confidenceLevel", t->score);
    cJSON_AddItemToObject(d, "coord", int_array(t->box, 4));
    static const int none[2] = {-1, -1};
    cJSON_AddItemToObject(d, "coord3d", int_array(none, 2));
    NUM(d, "firstShownTimeMs", (double)t->first_wall);
    NUM(d, "idleSinceTimeMs", 0);
    cJSON_AddArrayToObject(d, "intelligenceZones");
    cJSON_AddArrayToObject(d, "lines");
    cJSON_AddArrayToObject(d, "loiterZones");
    STR(d, "name", "");
    STR(d, "objectType", detect_class_name(t->cls));
    cJSON_AddArrayToObject(d, "secondLensZones");
    cJSON_AddBoolToObject(d, "stationary", 0);
    STR(d, "tag", "");
    NUM(d, "trackerID", t->id);
    cJSON *zones = cJSON_AddArrayToObject(d, "zones");
    for (int i = 0; i < s->set.nzones; i++)
        if (t->zones & (1 << i))
            cJSON_AddItemToArray(zones, cJSON_CreateNumber(s->set.zones[i].id));
    return d;
}

static int reported(const struct smart_track *t) { return t->id && t->confirmed && t->zones; }

static cJSON *payload(struct smart *s, const char *edge, int64_t mono, int64_t wall, int leave) {
    cJSON *p = cJSON_CreateObject();
    NUM(p, "clockMonotonic", (double)mono);
    NUM(p, "clockStream", (double)mono);
    NUM(p, "clockStreamRate", 1000);
    NUM(p, "clockWall", (double)wall);
    cJSON *descs = cJSON_AddArrayToObject(p, "descriptors");
    cJSON *types = cJSON_AddArrayToObject(p, "objectTypes");
    int seen_type[DETECT_CLASSES] = {0}, level[SMART_ZONES] = {0};
    for (int i = 0; i < SMART_TRACKS; i++) {
        struct smart_track *t = &s->tracks[i];
        if (!reported(t) || (!leave && mono - t->last_mono > LOST_MS))
            continue;
        // Zone level: the strongest object in it (at its best, on leave).
        int score = leave ? t->best_score : t->score;
        for (int z = 0; z < s->set.nzones; z++)
            if ((t->zones & (1 << z)) && score > level[z])
                level[z] = score;
        if (leave)
            continue;
        cJSON_AddItemToArray(descs, descriptor(s, t));
        if (!seen_type[t->cls]++)
            cJSON_AddItemToArray(types, cJSON_CreateString(detect_class_name(t->cls)));
    }
    NUM(p, "displayTimeoutMSec", DISPLAY_TIMEOUT_MS);
    STR(p, "edgeType", edge);
    NUM(p, "eventId", s->next_event++);
    STR(p, "smartDetectSnapshotFullFoV", leave ? s->fullfov : "");
    NUM(p, "smartDetectSnapshotFullFoVHeight", leave && *s->fullfov ? s->fullfov_h : 0);
    NUM(p, "smartDetectSnapshotFullFoVWidth", leave && *s->fullfov ? s->fullfov_w : 0);
    cJSON *snaps = cJSON_AddArrayToObject(p, "smartDetectSnapshots");
    if (leave) {
        // One crop per object of the event, from its best sighting, and
        // which type and zones each tracker was.
        cJSON *attrs = cJSON_AddObjectToObject(p, "trackerIDAttrMap");
        for (int i = 0; i < SMART_TRACKS; i++) {
            struct smart_track *t = &s->tracks[i];
            if (!reported(t) || !*t->snapshot)
                continue;
            cJSON *e = cJSON_CreateObject();
            NUM(e, "clockBestMonotonic", (double)t->best_mono);
            NUM(e, "clockBestWall", (double)t->best_wall);
            NUM(e, "confidenceLevel", t->best_score);
            cJSON_AddItemToObject(e, "coord", int_array(t->best_box, 4));
            cJSON_AddItemToObject(e, "framingRect", int_array(t->framing, 4));
            cJSON_AddBoolToObject(e, "reVerifyEligible", 0);
            STR(e, "smartDetectSnapshot", t->snapshot);
            NUM(e, "smartDetectSnapshotHeight", SMART_SNAPSHOT);
            STR(e, "smartDetectSnapshotName", "");
            STR(e, "smartDetectSnapshotType", detect_class_name(t->cls));
            NUM(e, "smartDetectSnapshotWidth", SMART_SNAPSHOT);
            NUM(e, "trackerID", t->id);
            cJSON_AddItemToArray(snaps, e);
            char key[16];
            snprintf(key, sizeof(key), "%u", t->id);
            cJSON *a = cJSON_AddObjectToObject(attrs, key);
            STR(a, "objectType", detect_class_name(t->cls));
            cJSON *zones = cJSON_AddArrayToObject(a, "zone");
            for (int z = 0; z < s->set.nzones; z++)
                if (t->zones & (1 << z))
                    cJSON_AddItemToArray(zones, cJSON_CreateNumber(s->set.zones[z].id));
        }
    }
    cJSON *zs = cJSON_AddObjectToObject(p, "zonesStatus");
    for (int z = 0; z < s->set.nzones; z++) {
        char key[16];
        snprintf(key, sizeof(key), "%d", s->set.zones[z].id);
        cJSON *o = cJSON_AddObjectToObject(zs, key);
        NUM(o, "level", level[z]);
        STR(o, "status", edge);
    }
    return p;
}

static struct smart_track *new_track(struct smart *s, int64_t mono) {
    struct smart_track *slot = NULL;
    for (int i = 0; i < SMART_TRACKS; i++) {
        struct smart_track *t = &s->tracks[i];
        // Free, or lost and not part of an ongoing event.
        if (!t->id || (mono - t->last_mono > LOST_MS && !(s->in_event && reported(t)))) {
            slot = t;
            break;
        }
    }
    if (slot) {
        memset(slot, 0, sizeof(*slot));
        slot->id = s->next_tracker++;
    }
    return slot;
}

void smart_update(struct smart *s, const struct detection *dets, int n, int64_t mono,
                  int64_t wall, int fov_w, int fov_h, struct smart_result *r) {
    memset(r, 0, sizeof(*r));
    int used[SMART_TRACKS] = {0};
    int pass_best = 0, pass_reported = 0;
    for (int k = 0; k < n; k++) {
        const struct detection *d = &dets[k];
        if (!s->set.enabled[d->cls])
            continue;
        int box[4] = {(int)(d->x * 1000), (int)(d->y * 1000), (int)(d->w * 1000), (int)(d->h * 1000)};
        int score = (int)(d->score * 100 + 0.5f);
        struct smart_track *t = NULL;
        int best = -1;
        for (int i = 0; i < SMART_TRACKS; i++) {
            struct smart_track *c = &s->tracks[i];
            if (!c->id || used[i] || c->cls != d->cls || mono - c->last_mono > LOST_MS)
                continue;
            int cost = match_cost(c, box);
            if (cost >= 0 && (best < 0 || cost < best)) {
                best = cost;
                t = c;
            }
        }
        if (!t && !(t = new_track(s, mono)))
            continue;
        used[t - s->tracks] = 1;
        if (!t->hits) {
            t->cls = d->cls;
            t->first_wall = wall;
        }
        memcpy(t->box, box, sizeof(box));
        t->score = score;
        t->hits++;
        t->last_mono = mono;
        t->zones = object_zones(s, d->cls, box, score);
        if (t->hits >= CONFIRM_HITS || score >= CONFIRM_SCORE)
            t->confirmed = 1;
        if (t->zones && score > t->best_score && r->nsnap < SMART_TRACKS) {
            t->best_score = score;
            memcpy(t->best_box, box, sizeof(box));
            framing_rect(box, fov_w, fov_h, t->framing);
            t->best_mono = mono;
            t->best_wall = wall;
            snprintf(t->snapshot, sizeof(t->snapshot), "smartdetectsnap_zone_%u%lld.jpg", t->id,
                     (long long)wall);
            r->snap_det[r->nsnap] = k;
            memcpy(r->framing[r->nsnap], t->framing, sizeof(t->framing));
            snprintf(r->snapshot_name[r->nsnap], sizeof(r->snapshot_name[0]), "%s", t->snapshot);
            r->nsnap++;
        }
        if (reported(t)) {
            pass_reported = 1;
            if (score > pass_best)
                pass_best = score;
        }
    }

    int live = 0;
    for (int i = 0; i < SMART_TRACKS; i++)
        live += reported(&s->tracks[i]) && mono - s->tracks[i].last_mono <= LOST_MS;

    if (!s->in_event && live) {
        s->in_event = 1;
        s->fullfov_score = 0;
        *s->fullfov = 0;
    }
    // The event's wide view: the frame of its best sighting so far.
    if (s->in_event && pass_reported && pass_best > s->fullfov_score) {
        s->fullfov_score = pass_best;
        s->fullfov_w = fov_w;
        s->fullfov_h = fov_h;
        snprintf(s->fullfov, sizeof(s->fullfov), "smartdetectsnap_zone_%08d_fullfov.jpg",
                 ++s->fullfov_seq);
        r->want_fullfov = 1;
        snprintf(r->fullfov_name, sizeof(r->fullfov_name), "%s", s->fullfov);
    }

    if (live && s->in_event == 1) {
        r->payload = payload(s, "enter", mono, wall, 0);
        s->in_event = 2;
    } else if (live && pass_reported) {
        r->payload = payload(s, "moving", mono, wall, 0);
    } else if (!live && s->in_event) {
        r->payload = payload(s, "leave", mono, wall, 1);
        s->in_event = 0;
        // The event's objects are done; later sightings start new ones.
        for (int i = 0; i < SMART_TRACKS; i++)
            if (reported(&s->tracks[i]))
                memset(&s->tracks[i], 0, sizeof(s->tracks[i]));
    }
}
