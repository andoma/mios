#pragma once

// Bind a virtual console to a pushpull channel.
//
// Console output arriving on the channel is appended to the vcon's
// scrollback; keystrokes from attached clients go back out on it. The
// intended use is a VLLP client channel opened against a remote unit's
// "shell" service, so `attach <unit>` on a gateway gives a console on the
// unit:
//
//   vcon_t *vc = vcon_create("unit1", 4096, 64);
//   vllp_t *v  = vllp_client_create(0x530, 0x531, 8, 3);
//   vllp_client_bind(v, "shell", unit_console_open, vc);
//
// ...where unit_console_open() is a one-liner calling this. Nothing here
// is VLLP-specific though: it is a pushpull app, and works over any
// transport that speaks pushpull.
//
// Spawns no thread, for any number of consoles.

#include <mios/error.h>

struct pushpull;
typedef struct vcon vcon_t;

// Suitable directly as a vllp_client_bind() open callback when the opaque
// is the vcon. Called again for every new session; each call binds a fresh
// app, and the previous one has already been closed.
error_t vcon_pushpull_open(vcon_t *vc, struct pushpull *pp);
