#ifndef UVC_DAYNIGHT_H
#define UVC_DAYNIGHT_H

#include "config.h"

// Day/night switching: the IR cut filter, the IR LEDs and the ISP's colour
// mode (capture_night), following Protect's IR settings and, in the auto
// modes, the ISP's exposure.
//
// Day to night: exposure above NIGHT_EXPOSURE for a few seconds. Night to
// day can't use the same reading while the IR LEDs are on: they light up
// whatever is near the lens (a hand over it reads brighter than daylight).
// So with the LEDs on uvcd probes: LEDs off for a few seconds, and day only
// if the scene stays below DAY_EXPOSURE without them. Probes back off while
// they keep failing, and come sooner when the exposure moves.

void daynight_start(const struct config *c);
void daynight_stop(void);

// Protect's IR settings in the form it sends cameras (ChangeIspSettings):
// irLedMode "manual" (always day, or always night with LEDs) or "auto";
// irLedLevel 0 means no LEDs; icrSwitchMode "lux" with icrCustomValue 0-10
// moves the switching point (2 is the default, higher switches to night in
// more light).
void daynight_settings(int manual, int leds, int lux, int custom);

#endif
