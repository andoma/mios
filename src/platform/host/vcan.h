#pragma once

/*
 * Virtual CAN/DSIG interface for test suites.
 *
 * The mios side is a can_netif like any real CAN driver, so the DSIG
 * stack (and VLLP on top of it) runs unmodified. The peer side is a
 * simulation thread (see cpu/host/sim.h) that receives the DSIG frames
 * mios transmits and injects frames back, through two rings and an IRQ
 * line, the way a CAN controller's mailboxes and interrupt work.
 *
 * A DSIG frame here is a (signal id, payload) pair -- the same thing
 * can.c puts on and takes off the wire as [u32 LE id][payload]. The peer
 * API exchanges id and payload separately so tests never hand-assemble
 * that prefix.
 *
 * Only available in virtual time mode (test suites).
 */

#include <stdint.h>
#include <stddef.h>
#include <sys/types.h>

struct can_netif;

typedef struct vcan vcan_t;

// mtu is the VLLP/DSIG payload MTU (8 for classic CAN, 64 for FDCAN).
vcan_t *vcan_create(const char *name, uint8_t mtu);

void vcan_set_link(vcan_t *v, int up);

struct can_netif *vcan_cni(vcan_t *v);

// ---- Peer side, simulation threads only ----

// Next DSIG frame transmitted by mios. Returns the payload length and
// writes the signal id to *id, or -1 if the deadline passed first.
ssize_t vcan_peer_recv(vcan_t *v, uint32_t *id, void *buf, size_t buflen,
                       uint64_t deadline);

// Deliver a DSIG frame to mios.
void vcan_peer_send(vcan_t *v, uint32_t id, const void *payload, size_t len);
