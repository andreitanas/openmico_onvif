#ifndef UVC_ISP_H
#define UVC_ISP_H

#include "cJSON.h"

// Protect's ISP settings (ChangeIspSettings): kept as Protect sent them,
// applied to the image (capture_isp) and day/night switching (daynight), and
// reported back.

void isp_init(void);
// Merge payload into the settings (reset: back to defaults first), apply
// them, and return them for the response.
cJSON *isp_change(const cJSON *payload, int reset);

#endif
