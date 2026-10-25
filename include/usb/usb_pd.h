#pragma once

// USB Power Delivery, sink side.
//
// The protocol layer (GoodCRC, MessageID, retries) runs in interrupt
// context on top of a Type-C PHY, the policy engine in its own thread.
// The Type-C layer starts it when attached as sink.

#include <stdint.h>

#include "typec.h"

typedef struct usb_pd usb_pd_t;

usb_pd_t *usb_pd_create(typec_phy_t *tp, const char *name);

// Request the highest fixed voltage at or below max_mv, and up to max_ma
// of current. Renegotiates right away if a contract is in place.
// The default is 5V, 500mA
void usb_pd_set_sink_policy(usb_pd_t *pd, int max_mv, int max_ma);

// Called by the Type-C layer
void usb_pd_attach(usb_pd_t *pd);
void usb_pd_detach(usb_pd_t *pd);

// True while a Hard Reset is in progress, VBUS goes away and comes back
// during it, which must not be taken as a detach
int usb_pd_hard_reset_active(usb_pd_t *pd);
