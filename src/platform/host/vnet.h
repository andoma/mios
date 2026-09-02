#pragma once

/*
 * Virtual Ethernet NIC for test suites.
 *
 * Mios side: an ether_netif like any other driver. Peer side: a
 * simulation thread (see cpu/host/sim.h) that receives the frames Mios
 * transmits and injects frames back, through two rings and an IRQ line,
 * the way a NIC's DMA rings and interrupt work.
 */

#include <stdint.h>
#include <stddef.h>
#include <sys/types.h>

struct ether_netif;

typedef struct vnet vnet_t;

vnet_t *vnet_create(const char *name, const uint8_t mac[6]);

void vnet_set_link(vnet_t *v, int up);

struct ether_netif *vnet_eni(vnet_t *v);

// ---- Peer side, simulation threads only ----

// Next frame transmitted by Mios, or -1 if the deadline passed first
ssize_t vnet_peer_recv(vnet_t *v, void *buf, size_t buflen, uint64_t deadline);

// Deliver a frame to Mios
void vnet_peer_send(vnet_t *v, const void *frame, size_t len);
