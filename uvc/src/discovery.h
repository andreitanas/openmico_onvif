#ifndef UVC_DISCOVERY_H
#define UVC_DISCOVERY_H

#include "config.h"

// Ubiquiti device discovery: Protect probes UDP port 10001 (broadcast and
// multicast 233.89.188.1) every minute, and lists devices that answer as
// unmanaged as ready to adopt. A thread answers as the model in c, managed
// once adopted, as real UniFi cameras do.
void discovery_start(const struct config *c);

#endif
