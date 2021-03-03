#include <string.h>
#include <stdio.h>
#include <mios/task.h>
#include <mios/timer.h>
#include <unistd.h>
#include <stdlib.h>

#include "irq.h"
#include "ether.h"
#include "net.h"
#include "ipv4.h"
#include "dhcpv4.h"

static const uint8_t ether_bcast[6] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff};



typedef struct ether_hdr {
  uint8_t dst_addr[6];
  uint8_t src_addr[6];
  uint16_t type;
} ether_hdr_t;



struct arp_pkt {
  uint16_t htype;
  uint16_t ptype;
  uint8_t hlen;
  uint8_t plen;
  uint16_t oper;
  uint8_t sha[6];
  uint32_t spa;
  uint8_t tha[6];
  uint32_t tpa;
} __attribute__((packed));



static void
ether_output(ether_netif_t *eni, pbuf_t *pb,
             uint16_t ether_type, const uint8_t *dstmac)
{
  pb = pbuf_prepend(pb, 14);

  uint8_t *eh = pbuf_data(pb, 0);
  memcpy(eh, dstmac, 6);
  memcpy(eh + 6, eni->eni_addr, 6);

  eh[12] = ether_type >> 8;
  eh[13] = ether_type;
  eni->eni_output(eni, pb, 0);
}



static void
arp_send_who_has(ether_netif_t *eni, uint32_t addr)
{
  pbuf_t *pb = pbuf_make(14);
  struct arp_pkt *ap = pbuf_append(pb, sizeof(struct arp_pkt));
  ap->htype = htons(1);
  ap->ptype = htons(ETHERTYPE_IPV4);
  ap->hlen = 6;
  ap->plen = 4;
  ap->oper = htons(1); // who has
  memcpy(ap->sha, eni->eni_addr, 6);
  ap->spa = eni->eni_ni.ni_ipv4_addr;
  memset(ap->tha, 0, 6);
  ap->tpa = addr;
  ether_output(eni, pb, ETHERTYPE_ARP, ether_bcast);
}


static pbuf_t *
arp_input(ether_netif_t *eni, pbuf_t *pb)
{
  if((pb = pbuf_pullup(pb, sizeof(struct arp_pkt))) == NULL)
    return pb;

  struct arp_pkt *ap = pbuf_data(pb, 0);

  if(ap->oper == htons(1) && ap->tpa == eni->eni_ni.ni_ipv4_addr) {

    // Request for our address
    // We reuse the packet for the reply

    ap->oper = htons(2);
    memcpy(ap->tha, ap->sha, 6);
    ap->tpa = ap->spa;

    memcpy(ap->sha, eni->eni_addr, 6);
    ap->spa = eni->eni_ni.ni_ipv4_addr;

    ether_output(eni, pb, ETHERTYPE_ARP, ap->tha);
    return NULL;
  } else if(ap->oper == htons(2)) {

    mutex_lock(&net_output_mutex);

    nexthop_t *nh;
    LIST_FOREACH(nh, &eni->eni_ni.ni_nexthops, nh_netif_link) {
      if(nh->nh_addr == ap->spa) {
        memcpy(nh->nh_hwaddr, ap->sha, 6);
        nh->nh_state = 255;
        if(nh->nh_pending) {
          ether_output(eni, nh->nh_pending, ETHERTYPE_IPV4, nh->nh_hwaddr);
          nh->nh_pending = NULL;
          break;
        }
      }
    }
    mutex_unlock(&net_output_mutex);
  }


  return pb;
}


static pbuf_t *
ether_input(ether_netif_t *eni, pbuf_t *pb)
{
  //  pbuf_print("ether", pb);

  if((pb = pbuf_pullup(pb, sizeof(ether_hdr_t))) == NULL)
    return pb;

  const ether_hdr_t *eh = pbuf_data(pb, 0);

  if(memcmp(&eh->dst_addr, eni->eni_addr, 6)) {
    if((eh->dst_addr[0] & 1) == 0) {
      // Not broadcast and not to us, drop
      return pb;
    }
  }

  const uint16_t etype = ntohs(eh->type);

  pb = pbuf_drop(pb, 14);

  switch(etype) {
  case ETHERTYPE_IPV4:
    return ipv4_input(&eni->eni_ni, pb);
  case ETHERTYPE_ARP:
    return arp_input(eni, pb);
  }
  return pb;
}

static void
nexthop_destroy(nexthop_t *nh)
{
  LIST_REMOVE(nh, nh_global_link);
  LIST_REMOVE(nh, nh_netif_link);
  free(nh);
}


static void
ether_nexthop_periodic(ether_netif_t *eni)
{
  mutex_lock(&net_output_mutex);

  nexthop_t *nh, *next;
  for(nh = LIST_FIRST(&eni->eni_ni.ni_nexthops); nh != NULL; nh = next) {
    next = LIST_NEXT(nh, nh_netif_link);

    nh->nh_state--;
    if(nh->nh_state == 0) {
      nexthop_destroy(nh);
    } else if(nh->nh_state < NEXTHOP_ACTIVE && nh->nh_in_use) {
      arp_send_who_has(eni, nh->nh_addr);
    }

    if(nh->nh_in_use)
      nh->nh_in_use--;

  }

  mutex_unlock(&net_output_mutex);
}





static void
ether_work(ether_netif_t *eni, uint16_t work_bits)
{
  if(work_bits & ETHER_NETIF_PERIODIC) {
    ether_nexthop_periodic(eni);
    dhcpv4_periodic(eni);
  }
}


static void __attribute__((noreturn))
ether_thread(void *arg)
{
  ether_netif_t *eni = arg;

  int q = irq_forbid(IRQ_LEVEL_NET);

  while(1) {

    if(eni->eni_work_bits) {
      const uint16_t work_bits = eni->eni_work_bits;
      eni->eni_work_bits = 0;
      irq_permit(q);
      ether_work(eni, work_bits);
      q = irq_forbid(IRQ_LEVEL_NET);
      continue;
    }

    pbuf_t *pb = pbuf_splice(&eni->eni_ni.ni_rx_queue);
    if(pb != NULL) {
      irq_permit(q);
      pb = ether_input(eni, pb);
      q = irq_forbid(IRQ_LEVEL_NET);
      if(pb)
        pbuf_free_irq_blocked(pb);
      continue;
    }
    task_sleep(&eni->eni_ni.ni_rx_waitable);
  }
}


static void
ether_ipv4_output(netif_t *ni, struct nexthop *nh, pbuf_t *pb)
{
  ether_netif_t *eni = (ether_netif_t *)ni;

  if(nh == NULL) {
    ether_output(eni, pb, 0x0800, ether_bcast);
    return;
  }

  nh->nh_in_use = 5;

  if(nh->nh_state <= NEXTHOP_RESOLVE) {

    if(nh->nh_state == 0) {
      nh->nh_state = NEXTHOP_RESOLVE;
      arp_send_who_has(eni, nh->nh_addr);
    } else {
      nh->nh_state--;
    }

    if(nh->nh_pending != NULL)
      pbuf_free(nh->nh_pending);
    nh->nh_pending = pb;
    return;
  }

  ether_output(eni, pb, 0x0800, nh->nh_hwaddr);
}


static void
periodic_timer_cb(void *opaque, uint64_t expire)
{
  ether_netif_t *eni = opaque;

  int q = irq_forbid(IRQ_LEVEL_NET);
  eni->eni_work_bits |= ETHER_NETIF_PERIODIC;
  task_wakeup(&eni->eni_ni.ni_rx_waitable, 0);
  irq_permit(q);
  timer_arm_abs(&eni->eni_periodic_timer, expire + 1000000, 0);
}

void
ether_netif_init(ether_netif_t *eni, const char *name)
{
  STAILQ_INIT(&eni->eni_ni.ni_rx_queue);
  eni->eni_ni.ni_ipv4_output = ether_ipv4_output;
  task_waitable_init(&eni->eni_ni.ni_rx_waitable, name);
  task_create((void *)ether_thread, eni, 512, name, 0, 4);

  mutex_lock(&net_output_mutex);
  LIST_INSERT_HEAD(&netifs, &eni->eni_ni, ni_global_link);
  mutex_unlock(&net_output_mutex);

  eni->eni_periodic_timer.t_cb = periodic_timer_cb;
  eni->eni_periodic_timer.t_opaque = eni;

  timer_arm_abs(&eni->eni_periodic_timer, clock_get() + 1000000, 0);
}
