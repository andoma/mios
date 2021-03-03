#pragma once

#include <mios/task.h>

#include "pbuf.h"

#include "ipv4.h"

LIST_HEAD(netif_list, netif);
LIST_HEAD(nexthop_list, nexthop);

struct nexthop;

extern struct netif_list netifs;
extern mutex_t net_output_mutex;

typedef struct netif {

  struct pbuf_queue ni_rx_queue;
  task_waitable_t ni_rx_waitable;
  task_t *ni_rx_task;

  LIST_ENTRY(netif) ni_global_link;
  uint32_t ni_ipv4_addr;  // Our address
  uint8_t ni_ipv4_prefixlen;

  struct nexthop_list ni_nexthops;

  void (*ni_ipv4_output)(struct netif *ni, struct nexthop *nh, pbuf_t *pb);

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
