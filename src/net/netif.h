#pragma once

#include <mios/device.h>
#include <mios/task.h>

#include "pbuf.h"
#include "net_task.h"

SLIST_HEAD(netif_list, netif);
LIST_HEAD(nexthop_list, nexthop);

struct nexthop;

extern struct netif_list netifs;
extern mutex_t netif_mutex;

#define NETIF_TASK_RX 0x1


typedef struct netif {

  device_t ni_dev;

  net_task_t ni_task;

  struct pbuf_queue ni_rx_queue;

  struct nexthop_list ni_nexthops;

  // FIXME: This needs to be reworks to support multiple addresses
  // and address families per interface
  uint32_t ni_local_addr;  // Our address
  uint8_t ni_local_prefixlen;
  uint8_t ni_ifindex;
  uint16_t ni_mtu;

  uint32_t ni_pending_signals;

  pbuf_t *(*ni_output)(struct netif *ni, struct nexthop *nh, pbuf_t *pb);

  void (*ni_buffers_avail)(struct netif *ni);

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



void netif_attach(netif_t *ni, const char *name, const device_class_t *dc);

void netlog(const char *fmt, ...);

void netlog_hexdump(const char *prefix, const uint8_t *buf, size_t len);

static inline void netif_wakeup(netif_t *ni)
{
  net_task_raise(&ni->ni_task, NETIF_TASK_RX);
}
