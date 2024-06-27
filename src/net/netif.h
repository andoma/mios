#pragma once

#include <mios/device.h>
#include <mios/task.h>

#include "pbuf.h"
#include "net_task.h"

SLIST_HEAD(netif_list, netif);
LIST_HEAD(nexthop_list, nexthop);

struct nexthop;

#define NETIF_TASK_RX          0x1
#define NETIF_TASK_STATUS_UP   0x2
#define NETIF_TASK_STATUS_DOWN 0x4

#define NETIF_F_UP   0x1

#define NETIF_F_RX_IPV4_CKSUM_OFFLOAD 0x10
#define NETIF_F_RX_ICMP_CKSUM_OFFLOAD 0x20
#define NETIF_F_RX_UDP_CKSUM_OFFLOAD  0x40
#define NETIF_F_RX_TCP_CKSUM_OFFLOAD  0x80

#define NETIF_F_TX_IPV4_CKSUM_OFFLOAD 0x100
#define NETIF_F_TX_ICMP_CKSUM_OFFLOAD 0x200
#define NETIF_F_TX_UDP_CKSUM_OFFLOAD  0x400
#define NETIF_F_TX_TCP_CKSUM_OFFLOAD  0x800


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

  uint32_t ni_flags;

  uint32_t ni_pending_signals;

  pbuf_t *(*ni_output)(struct netif *ni, struct nexthop *nh, pbuf_t *pb);

  void (*ni_buffers_avail)(struct netif *ni);

  struct pbuf *(*ni_input)(struct netif *ni, struct pbuf *pb);

  void (*ni_status_change)(struct netif *ni);

  SLIST_ENTRY(netif) ni_global_link;

#ifdef ENABLE_NET_DSIG

  pbuf_t *(*ni_dsig_output)(struct netif *ni, pbuf_t *pb, uint32_t group,
                            uint32_t flags);
  const struct dsig_output_filter *ni_dsig_output_filter;
#endif

} netif_t;


extern struct netif_list netifs;


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

void netif_detach(netif_t *ni);

netif_t *netif_get_net(netif_t *cur);

void netlog(const char *fmt, ...);

void netlog_hexdump(const char *prefix, const uint8_t *buf, size_t len);

static inline void netif_wakeup(netif_t *ni)
{
  net_task_raise(&ni->ni_task, NETIF_TASK_RX);
}
