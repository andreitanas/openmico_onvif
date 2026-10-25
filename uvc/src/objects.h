#ifndef UVC_OBJECTS_H
#define UVC_OBJECTS_H

#include <stddef.h>
#include <stdint.h>

#include "analytics.h"
#include "cJSON.h"

// Smart detections: while there is motion, a low-priority thread runs the
// object detector on the moving area of the capture's raw low-resolution
// frames and reports people, vehicles and animals to Protect as
// EventSmartDetect.

// model: path without .param/.bin. Returns -1 (and detection stays off) if
// it cannot be loaded.
int objects_start(analytics_send_fn send_event, const char *model, int width, int height);

// From motion detection: something moved in box (x, y, w, h, 0-1000).
void objects_motion(const int *box, int64_t mono_ms);

// ChangeSmartDetectSettings: enabled types and zones.
void objects_settings(const cJSON *payload);

// A snapshot named in an EventSmartDetect, by file name. 0 with a malloc'd
// copy, -1 if unknown.
int objects_file(const char *filename, uint8_t **data, size_t *len);

#endif
