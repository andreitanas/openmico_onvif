#ifndef UVC_AVCLIENT_H
#define UVC_AVCLIENT_H

#include "cJSON.h"
#include "config.h"
#include "source.h"

// Speaks Protect's camera protocol (ubnt_avclient over wss://nvr:7442) and
// drives the video streams it asks for. Never returns.
void avclient_run(struct config *cfg, struct source *hi, struct source *lo);
// Send an event (e.g. EventSmartMotion) from any thread; takes ownership of
// payload. Dropped while not connected to Protect.
void avclient_event(const char *function, cJSON *payload);

#endif
