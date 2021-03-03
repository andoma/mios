#pragma once

#include <mios/timer.h>

#include "netif.h"

#define ETHERTYPE_IPV4 0x0800
#define ETHERTYPE_ARP  0x0806

#define ETHER_NETIF_PERIODIC 0x1


typedef struct ether_netif {
  netif_t eni_ni;
  void (*eni_output)(struct ether_netif *eni, pbuf_t *pb, int flags);

  uint16_t eni_work_bits; // Protected at IRQ_LEVEL_NET
  uint8_t eni_addr[6];    // Our address

  uint8_t eni_dhcp_state;
  uint32_t eni_dhcp_xid;
  uint32_t eni_dhcp_server_ip;
  uint32_t eni_dhcp_requested_ip;
  uint64_t eni_dhcp_timeout;

} ether_netif_t;


void ether_netif_init(ether_netif_t *eni, const char *name);
