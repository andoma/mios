/*
 * UDP/multicast transport for host-side DSIG.
 *
 * Wire format matches src/net/dsig_udp.c on the guest: [u32 LE signal][payload]
 * inside a UDP datagram. Defaults: multicast group 239.255.213.22, port 0xd516.
 *
 * Usage:
 *   dsig_udp_t *udp = dsig_udp_create(NULL, 0, NULL);
 *   dsig_t *bus = dsig_create(dsig_udp_tx, udp);
 *   dsig_udp_start(udp, bus);
 *   ... use the bus ...
 *   dsig_udp_destroy(udp);
 *   dsig_destroy(bus);
 *
 * Link with: host/dsig.c, host/dsig_udp.c, -lpthread
 */

#pragma once

#include <stddef.h>
#include <stdint.h>

#include "dsig.h"

#ifdef __cplusplus
extern "C" {
#endif

#define DSIG_UDP_DEFAULT_GROUP "239.255.213.22"
#define DSIG_UDP_DEFAULT_PORT  0xd516

typedef struct dsig_udp dsig_udp_t;

/* Create a UDP transport joined on (group, port). If bind_ifname is non-NULL,
 * the multicast group is joined on that specific interface and outgoing
 * multicast goes out the same interface. Pass NULL group or 0 port to use
 * defaults. Returns NULL on failure.
 */
dsig_udp_t *dsig_udp_create(const char *group, uint16_t port,
                            const char *bind_ifname);

/* Spawns the rx thread, which calls dsig_input(bus, ...) for every
 * received datagram with a valid header. Call once, after dsig_create()
 * has been wired with dsig_udp_tx + this transport as the opaque.
 * Returns 0 on success, -1 on failure.
 */
int dsig_udp_start(dsig_udp_t *t, dsig_t *bus);

/* Stops the rx thread, closes the socket, frees the transport. Safe to
 * call before or after dsig_udp_start().
 */
void dsig_udp_destroy(dsig_udp_t *t);

/* TX callback matching dsig_tx_fn. Pass this (and the dsig_udp_t * as
 * opaque) to dsig_create().
 */
void dsig_udp_tx(void *opaque, uint32_t signal, const void *data, size_t len);

#ifdef __cplusplus
}
#endif
