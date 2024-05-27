#include "udp.h"

#include "net/pbuf.h"
#include "net/ether.h"
#include "net/ipv4/ipv4.h"
#include "net/ipv4/udp.h"
#include "net/net.h"
#include "net/netif.h"
#include "net/net_task.h"

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <malloc.h>

#include <sys/param.h>

#include <mios/cli.h>
#include <mios/datetime.h>
#include <mios/eventlog.h>

#define IGMP_QUERY     0x11
#define IGMP_V1_REPORT 0x12
#define IGMP_V2_REPORT 0x16
#define IGMP_V3_REPORT 0x22
#define IGMP_LEAVE     0x17

#define IGMP_QUERY_ADDR htonl(0xe0000001u)
#define IGMP_LEAVE_ADDR htonl(0xe0000002u)
#define IGMP_V3_ADDR    htonl(0xe0000016u)

#define IGMP_NETTASK_RESCHEDULE  0x1
#define IGMP_NETTASK_CLEAN_GROUP 0x2

// Report interval when no query is seen.
#define IGMP_UNSOLICITED_REPORT_TIME 10000000 // 10 seconds

typedef struct igmp_packet {
  uint8_t type;
  uint8_t resp_time;
  uint16_t checksum;
  uint32_t group;
} igmp_packet_t __attribute__((packed, aligned(4)));

#define IGMP_PBUF_HEADROOM (16 + sizeof(ipv4_header_t) + sizeof(igmp_packet_t))

typedef struct igmp_group {
  LIST_ENTRY(igmp_group) ig_link;
  uint32_t ig_group;
  timer_t ig_timer;
  int ig_unsolicited;
} igmp_group_t;

static struct {
  mutex_t igmp_work_m;
  LIST_HEAD(, igmp_group) igmp_groups;
  LIST_HEAD(, igmp_group) igmp_freelist;
} g_igmp;

static void igmp_output(uint32_t group, uint32_t dst,
                        uint8_t type);

static void
igmp_cb(struct net_task *nt, uint32_t signals)
{
  igmp_group_t *g;
  mutex_lock(&g_igmp.igmp_work_m);
  if(signals & IGMP_NETTASK_RESCHEDULE) {
    LIST_FOREACH(g, &g_igmp.igmp_groups, ig_link) {
      if(g->ig_unsolicited)
        net_timer_arm(&g->ig_timer, clock_get() + IGMP_UNSOLICITED_REPORT_TIME);
    }
  }
  if(signals & IGMP_NETTASK_CLEAN_GROUP) {
    while((g = LIST_FIRST(&g_igmp.igmp_freelist))) {
      timer_disarm(&g->ig_timer);
      LIST_REMOVE(g, ig_link);
      free(g);
    }
  }
  mutex_unlock(&g_igmp.igmp_work_m);
}

static net_task_t igmp_task = { igmp_cb };

static void
igmp_timer_cb(void *opaque, uint64_t expire)
{
  igmp_group_t *g = opaque;
  igmp_output(g->ig_group, g->ig_group, IGMP_V2_REPORT);
  if(g->ig_unsolicited) {
    net_timer_arm(&g->ig_timer, clock_get() + IGMP_UNSOLICITED_REPORT_TIME);
  }
}

int
igmp_group_join(uint32_t group)
{
  igmp_group_t *g;
  mutex_lock(&g_igmp.igmp_work_m);
  LIST_FOREACH(g, &g_igmp.igmp_groups, ig_link) {
    if(g->ig_group == group) {
      break;
    }
  }

  if(!g) {
    evlog(LOG_DEBUG, "IGMP: Joining %Id", group);
    g = xalloc(sizeof(*g), 0, MEM_MAY_FAIL);
    if(!g) {
      mutex_unlock(&g_igmp.igmp_work_m);
      return ERR_NO_MEMORY;
    }
    g->ig_timer = (timer_t){
      .t_cb = igmp_timer_cb,
      .t_opaque = g,
      .t_name = "IGMP",
    };
    g->ig_group = group;
    LIST_INSERT_HEAD(&g_igmp.igmp_groups, g, ig_link);
  }
  // Until we have report a query, run unsolicited
  g->ig_unsolicited = 1;
  mutex_unlock(&g_igmp.igmp_work_m);
  net_task_raise(&igmp_task, IGMP_NETTASK_RESCHEDULE);
  return 0;
}

static void
igmp_output(uint32_t group, uint32_t dst, uint8_t type)
{
  pbuf_t *pb = pbuf_make(IGMP_PBUF_HEADROOM, 1);
  netif_t *ni = SLIST_FIRST(&netifs);

  pb = pbuf_prepend(pb, sizeof(igmp_packet_t), 1, 0);
  igmp_packet_t *iq = pbuf_data(pb, 0);
  iq->group = group;
  iq->type = type;
  iq->resp_time = 0;
  iq->checksum = 0;
  iq->checksum = ipv4_cksum_pbuf(0, pb, 0, sizeof(igmp_packet_t));

  pb = pbuf_prepend(pb, sizeof(ipv4_header_t), 1, 0);
  ipv4_header_t *ip = pbuf_data(pb, 0);

  ip->ver_ihl = 0x45;
  ip->tos = 0;
  ip->total_length = htons(pb->pb_pktlen);
  ip->id = rand();
  ip->fragment_info = 0;
  ip->ttl = 1;
  ip->proto = IPPROTO_IGMP;
  ip->src_addr = ni->ni_local_addr;
  ip->dst_addr = dst;

  ip->cksum = 0;
  if(!(ni->ni_flags & NETIF_F_TX_IPV4_CKSUM_OFFLOAD)) {
    ip->cksum = ipv4_cksum_pbuf(0, pb, 0, sizeof(ipv4_header_t));
  }
  dst = ntohl(dst);
  nexthop_t nh = {
    .nh_state = NEXTHOP_ACTIVE,
    .nh_hwaddr = { 0x01, 0x00, 0x5e,
      0x7f & (dst >> 16),
      dst >> 8,
      dst,
    },
  };

  ni->ni_output(ni, &nh, pb);
}

void
igmp_group_leave(uint32_t group)
{
  igmp_group_t *g = NULL;

  mutex_lock(&g_igmp.igmp_work_m);
  LIST_FOREACH(g, &g_igmp.igmp_groups, ig_link) {
    if(g->ig_group != group)
      continue;
    evlog(LOG_DEBUG, "IGMP: Leaving %Id", group);
    LIST_REMOVE(g, ig_link);
    igmp_output(group, IGMP_LEAVE_ADDR, IGMP_LEAVE);
    LIST_INSERT_HEAD(&g_igmp.igmp_freelist, g, ig_link);
    net_task_raise(&igmp_task, IGMP_NETTASK_CLEAN_GROUP);
    break;
  }
  mutex_unlock(&g_igmp.igmp_work_m);
}

pbuf_t *
igmp_input_ipv4(netif_t *ni, pbuf_t *pb, size_t igmp_offset)
{
  const ipv4_header_t *ip = pbuf_data(pb, 0);
  const igmp_packet_t *ih = (void *)(ip + 1);

  if(!(pb->pb_flags & PBUF_MCAST)) {
    return pb;
  }
  switch(ih->type) {
  case IGMP_QUERY: {
    // General qyery
    const int64_t rand_time = (int64_t)(ih->resp_time - 1) * rand() / RAND_MAX;
    const int64_t resp_time = clock_get() + (rand_time + 1) * 100000ll;
    mutex_lock(&g_igmp.igmp_work_m);
    igmp_group_t *g;
    LIST_FOREACH(g, &g_igmp.igmp_groups, ig_link) {
      g->ig_unsolicited = 0;
      if(ip->dst_addr == IGMP_QUERY_ADDR) {
        net_timer_arm(&g->ig_timer, resp_time);
      } else if(ih->group == g->ig_group) {
        net_timer_arm(&g->ig_timer, resp_time);
        break;
      }
    }
    mutex_unlock(&g_igmp.igmp_work_m);

    break;
  }
  default:
    break;
  }
  return pb;
}
