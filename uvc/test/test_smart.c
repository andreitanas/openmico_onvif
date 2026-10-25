// Smart-detect tracking and Protect payloads on synthetic detector passes.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "smart.h"

static int failures;

static void expect(const char *what, int ok) {
    printf("%-58s %s\n", what, ok ? "ok" : "FAILED");
    failures += !ok;
}

static const char *edge(const struct smart_result *r) {
    cJSON *e = r->payload ? cJSON_GetObjectItem(r->payload, "edgeType") : NULL;
    return e ? e->valuestring : "-";
}

static int count(cJSON *p, const char *key) {
    return cJSON_GetArraySize(cJSON_GetObjectItem(p, key));
}

int main(void) {
    static struct smart s;
    struct smart_result r;
    int64_t mono = 100000, wall = 1792900000000;
    smart_init(&s);

    // A one-off weak detection never becomes an event.
    struct detection weak = {DETECT_PERSON, 0.36f, 0.4f, 0.4f, 0.05f, 0.2f};
    smart_update(&s, &weak, 1, mono, wall, 640, 360, &r);
    expect("single weak detection: no payload", !r.payload);
    for (int i = 0; i < 6; i++) {
        mono += 1000, wall += 1000;
        smart_update(&s, NULL, 0, mono, wall, 640, 360, &r);
        expect("  ticks after it: still nothing", !r.payload);
        cJSON_Delete(r.payload);
    }

    // A person walking across: enter on the second sighting, then moving.
    const char *edges[6];
    int snaps = 0, fullfov = 0;
    uint32_t tracker = 0;
    for (int i = 0; i < 6; i++) {
        mono += 1200, wall += 1200;
        struct detection p = {DETECT_PERSON, 0.50f + 0.05f * (i == 3), 0.1f + 0.1f * i, 0.3f, 0.06f,
                              0.3f};
        smart_update(&s, &p, 1, mono, wall, 640, 360, &r);
        edges[i] = r.payload ? strdup(edge(&r)) : "-";
        snaps += r.nsnap;
        fullfov += r.want_fullfov;
        if (r.payload && !tracker) {
            cJSON *d = cJSON_GetArrayItem(cJSON_GetObjectItem(r.payload, "descriptors"), 0);
            tracker = (uint32_t)cJSON_GetObjectItem(d, "trackerID")->valuedouble;
        }
        if (i == 1 && r.payload) {
            char *text = cJSON_PrintUnformatted(r.payload);
            printf("  enter: %.400s...\n", text);
            free(text);
        }
        cJSON_Delete(r.payload);
    }
    expect("walking person: first sighting silent", !strcmp(edges[0], "-"));
    expect("  enter on the second", !strcmp(edges[1], "enter"));
    expect("  then moving", !strcmp(edges[2], "moving") && !strcmp(edges[5], "moving"));
    expect("  new best crops requested (first, then the 0.55 one)", snaps == 2);
    expect("  full-FoV kept for the event's best pass", fullfov == 2);
    expect("  one stable trackerID", tracker > 0);

    // Gone: leave after the lost timeout, with the crop and full FoV.
    cJSON *leave = NULL;
    for (int i = 0; i < 6 && !leave; i++) {
        mono += 1000, wall += 1000;
        smart_update(&s, NULL, 0, mono, wall, 640, 360, &r);
        leave = r.payload;
    }
    expect("person gone: leave", leave && !strcmp(cJSON_GetObjectItem(leave, "edgeType")->valuestring, "leave"));
    if (leave) {
        char *text = cJSON_PrintUnformatted(leave);
        printf("  leave: %.700s...\n", text);
        free(text);
        cJSON *snap = cJSON_GetArrayItem(cJSON_GetObjectItem(leave, "smartDetectSnapshots"), 0);
        expect("  one object crop, scored at its best (55)",
               count(leave, "smartDetectSnapshots") == 1 &&
                   cJSON_GetObjectItem(snap, "confidenceLevel")->valueint == 55);
        expect("  trackerIDAttrMap names it",
               cJSON_GetObjectItem(cJSON_GetObjectItem(leave, "trackerIDAttrMap"), "1") ||
                   cJSON_GetArraySize(cJSON_GetObjectItem(leave, "trackerIDAttrMap")) == 1);
        expect("  full FoV named", *cJSON_GetObjectItem(leave, "smartDetectSnapshotFullFoV")->valuestring);
        expect("  no descriptors", count(leave, "descriptors") == 0);
        cJSON *z = cJSON_GetObjectItem(cJSON_GetObjectItem(leave, "zonesStatus"), "1");
        expect("  zone level from the best sighting",
               z && cJSON_GetObjectItem(z, "level")->valueint == 55);
        cJSON_Delete(leave);
    }

    // Zone allowing only people: a car in it is not reported.
    struct smart_settings set = {.enabled = {1, 1, 1}, .nzones = 1};
    set.zones[0] = (struct motion_zone){.id = 3, .npts = 4, .level = 50};
    int xs[4] = {0, 1000, 1000, 0}, ys[4] = {0, 0, 1000, 1000};
    memcpy(set.zones[0].x, xs, sizeof(xs));
    memcpy(set.zones[0].y, ys, sizeof(ys));
    set.zone_types[0][DETECT_PERSON] = 1;
    smart_configure(&s, &set);
    int any = 0;
    for (int i = 0; i < 4; i++) {
        mono += 1200, wall += 1200;
        struct detection car = {DETECT_VEHICLE, 0.8f, 0.5f, 0.5f, 0.2f, 0.1f};
        smart_update(&s, &car, 1, mono, wall, 640, 360, &r);
        any |= r.payload != NULL;
        cJSON_Delete(r.payload);
    }
    expect("car in a person-only zone: no event", !any);

    return failures ? 1 : 0;
}
