#ifndef UVC_ANALYTICS_H
#define UVC_ANALYTICS_H

#include <stddef.h>
#include <stdint.h>

#include "cJSON.h"

// Motion events for Protect from the capture's raw low-resolution frames.
// Events go out through send_event (thread-safe; dropped while Protect is
// not connected).
typedef void (*analytics_send_fn)(const char *function, cJSON *payload);
// width x height: the largest low-resolution frame capture_raw may return.
void analytics_start(analytics_send_fn send_event, int width, int height);

// ChangeSmartMotionSettings: zones, sensitivity and timing.
void analytics_settings(const cJSON *payload);

// Files Protect fetches with GetRequest after an event (its "what" or
// "filename"): the motion snapshot and heatmap. Returns 0 with a malloc'd
// copy and its file name, -1 if the request is not for one of them.
int analytics_file(const char *what, const char *filename, uint8_t **data, size_t *len,
                   const char **name);

#endif
