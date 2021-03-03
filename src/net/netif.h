#pragma once

#include <mios/task.h>

#include "pbuf.h"

#define NET_MAX_INTERFACES 8

SLIST_HEAD(netif_list, netif);
LIST_HEAD(nexthop_list, nexthop);

#define NETIF_TYPE_ETHERNET 1

struct nexthop;

extern struct netif_list netifs;

typedef struct netif {

  struct pbuf_queue ni_rx_queue;

  uint32_t ni_ipv4_addr;  // Our address
  uint8_t ni_ipv4_prefixlen;
  uint8_t ni_ifindex;
  uint8_t ni_iftype;

  struct nexthop_list ni_nexthops;

  void (*ni_ipv4_output)(struct netif *ni, struct nexthop *nh, pbuf_t *pb);

  void (*ni_periodic)(struct netif *ni);
  struct pbuf *(*ni_input)(struct netif *ni, struct pbuf *pb);

  SLIST_ENTRY(netif) ni_global_link;

} netif_t;



#define NEXTHOP_IDLE       0
#define NEXTHOP_RESOLVE    5
#define NEXTHOP_ACTIVE     10

typedef struct nexthop {
  LIST_ENTRY(nexthop) nh_global_link;
  uint32_t nh_addr;

  LIST_ENTRY(nexthop) nh_netif_link;
  struct netif *nh_netif;

  struct pbuf *nh_pending;

  uint8_t nh_state;
  uint8_t nh_in_use;
  uint8_t nh_hwaddr[6];

} nexthop_t;


void netif_wakeup(netif_t *ni);

void netif_attach(netif_t *ni);

extern mutex_t netif_mutex;
