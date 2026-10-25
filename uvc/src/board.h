#ifndef UVC_BOARD_H
#define UVC_BOARD_H

#include <stdint.h>

// The camera board around uvcd: hardware watchdog and status LEDs.

// Arm the hardware watchdog (timeout_s) and keep it fed from a thread for as
// long as healthy() returns nonzero. Without a watchdog device it does
// nothing.
void board_watchdog_start(int timeout_s, int (*healthy)(void));
// Disarm it (magic close) before a deliberate exit.
void board_watchdog_stop(void);

// Status LEDs: blue while connected to Protect, yellow otherwise; all off
// when Protect turns the status light off.
void board_led_connected(int connected);
void board_led_enable(int enable);

#endif
