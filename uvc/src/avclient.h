#ifndef UVC_AVCLIENT_H
#define UVC_AVCLIENT_H

#include "config.h"
#include "source.h"

// Speaks Protect's camera protocol (ubnt_avclient over wss://nvr:7442) and
// drives the video streams it asks for. Never returns.
void avclient_run(struct config *cfg, struct source *hi, struct source *lo);

#endif
