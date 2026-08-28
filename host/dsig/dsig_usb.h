/*
 * USB transport for host-side DSIG. Talks to a mios usb-dsig endpoint
 * (src/lib/usb/usb_dsig.c on the guest): a vendor-class bulk interface
 * that frames each signal as a 2-byte header (12-bit id + flags,
 * CAN-like) followed by the payload, one signal per USB packet
 * (fragmentation is not implemented on the guest side, so neither is
 * it here).
 *
 * Reconnects automatically if the device is unplugged and replugged.
 *
 * Usage:
 *   dsig_usb_t *u = dsig_usb_create(0x6666, 0x0600, 0x02);
 *   dsig_t *bus = dsig_create(dsig_usb_tx, u);
 *   dsig_usb_start(u, bus);
 *   ...
 *   dsig_usb_destroy(u);
 *   dsig_destroy(bus);
 *
 * Link with: host/dsig/dsig.c, host/dsig/dsig_usb.c, -lusb-1.0 -lpthread
 */

#pragma once

#include <stddef.h>
#include <stdint.h>

#include "dsig.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct dsig_usb dsig_usb_t;

/* pid == 0 matches any product ID under vid. subclass is the vendor
 * interface subclass the guest passed to usb_dsig_create().
 */
dsig_usb_t *dsig_usb_create(uint16_t vid, uint16_t pid, uint8_t subclass);

int dsig_usb_start(dsig_usb_t *t, dsig_t *bus);

void dsig_usb_destroy(dsig_usb_t *t);

void dsig_usb_tx(void *opaque, uint32_t signal,
                 const void *data, size_t len);

/* 1 if a device is currently open and being read from. */
int dsig_usb_connected(dsig_usb_t *t);

#ifdef __cplusplus
}
#endif
