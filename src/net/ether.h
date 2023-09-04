#pragma once

#include <mios/timer.h>

#include "netif.h"

#define ETHERTYPE_IPV4 0x0800
#define ETHERTYPE_ARP  0x0806

typedef struct ether_netif {
  netif_t eni_ni;

  SLIST_ENTRY(ether_netif) eni_global_link;

  void (*eni_output)(struct ether_netif *eni, pbuf_t *pb, int flags);

  uint8_t eni_addr[6];    // Our address

  timer_t eni_periodic;

  uint8_t eni_dhcp_state;
  uint8_t eni_dhcp_retries;
  uint32_t eni_dhcp_xid;
  uint32_t eni_dhcp_server_ip;
  uint32_t eni_dhcp_requested_ip;
  timer_t eni_dhcp_timer;

} ether_netif_t;


void ether_netif_init(ether_netif_t *eni, const char *name,
                      const device_class_t *dc);

SLIST_HEAD(ether_netif_list, ether_netif);

extern struct ether_netif_list ether_netifs;

void ether_print(ether_netif_t *en, struct stream *st);
