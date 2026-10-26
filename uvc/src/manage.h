#ifndef UVC_MANAGE_H
#define UVC_MANAGE_H

#include "config.h"

// Protect's adopt request: clicking Adopt on a discovered camera makes
// Protect POST https://<camera>/api/1.2/manage with
//   {"username", "password", "mgmt": {"token", "hosts": ["ip:7442", ...],
//    "protocol": "wss"}}
// (camera firmware >= 4.9.2, see FWVERSION in discovery.c). A thread
// serves it on port 443: always while not adopted, and once adopted only
// with the password Protect set (it re-adopts cameras that stay
// disconnected).

void manage_start(const struct config *c);
// A request arrived and was not taken yet.
int manage_pending(void);
// Apply a pending request to c (config_manage). 1 if there was one. Called
// by the thread that owns c's Protect settings.
int manage_take(struct config *c);

#endif
