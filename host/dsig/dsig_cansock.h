/*
 * Linux SocketCAN transport for host-side DSIG. Port of dsig_cansock.cpp
 * from the TheRigg reference. Uses CAN-FD raw frames (CAN id = signal,
 * frame data = payload).
 *
 * Usage:
 *   dsig_cansock_t *cs = dsig_cansock_create(NULL);
 *   dsig_t *bus = dsig_create(dsig_cansock_tx, cs);
 *   dsig_cansock_start(cs, bus);
 *   ...
 *   dsig_cansock_destroy(cs);
 *   dsig_destroy(bus);
 *
 * Link with: host/dsig.c, host/dsig_cansock.c, -lpthread
 */

#pragma once

#include <stddef.h>
#include <stdint.h>

#include "dsig.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct dsig_cansock dsig_cansock_t;

/* Open a CAN raw socket bound to ifname. If ifname is NULL the IFC env
 * variable is consulted, falling back to "can0". Returns NULL on failure.
 */
dsig_cansock_t *dsig_cansock_create(const char *ifname);

int dsig_cansock_start(dsig_cansock_t *t, dsig_t *bus);

void dsig_cansock_destroy(dsig_cansock_t *t);

void dsig_cansock_tx(void *opaque, uint32_t signal,
                     const void *data, size_t len);

#ifdef __cplusplus
}
#endif
