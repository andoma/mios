#pragma once

#include <stddef.h>
#include <stdint.h>

struct ether_netif;

// Copy a received Ethernet frame into a pbuf chain and queue it on the
// netif. Caller must hold irq_forbid(IRQ_LEVEL_NET) and call
// netif_wakeup() afterwards.
void host_ether_rx(struct ether_netif *eni, const uint8_t *frame, size_t len);
