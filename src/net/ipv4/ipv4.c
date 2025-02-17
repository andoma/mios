#include <sys/param.h>

#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <malloc.h>

#include "net/netif.h"
#include "net/net.h"
#include "igmp.h"
#include "ipv4.h"
#include "irq.h"
#include "udp.h"
#include "tcp.h"

static uint32_t
ipv4_cksum_add(uint32_t sum, const uint16_t *u16, size_t len)
{
  while(len > 1) {
    sum += *u16++;
    len -= 2;
  }

  if(len)
    sum += *(uint8_t *)u16;

  return sum;
}


struct ipv4_cksum_pseudo_hdr {
  union {
    struct {
      uint32_t src_addr;
      uint32_t dst_addr;
      uint8_t zero;
      uint8_t protocol;
      uint16_t length;
    };
    uint16_t raw[6];
  };
};

uint32_t
ipv4_cksum_pseudo(uint32_t src_addr, uint32_t dst_addr,
                  uint8_t protocol, uint16_t length)
{
  struct ipv4_cksum_pseudo_hdr h = {
    {{src_addr, dst_addr, 0, protocol, htons(length)}}
  };
  return ipv4_cksum_add(0, h.raw, 12);
}


static uint16_t
ipv4_cksum_fold(uint32_t sum)
{
  while(sum > 0xffff)
    sum = (sum >> 16) + (sum & 0xffff);
  return sum;
}


uint16_t
ipv4_cksum_pbuf(uint32_t sum, pbuf_t *pb, int offset, int length)
{
  for(; pb != NULL; pb = pb->pb_next) {
    if(length == 0)
      break;

    if(offset > pb->pb_buflen) {
      offset -= pb->pb_buflen;
      continue;
    }

    int clen = MIN(length, pb->pb_buflen - offset);
    sum = ipv4_cksum_add(sum, pb->pb_data + pb->pb_offset + offset, clen);
    length -= clen;
    offset = 0;
  }
  return ~ipv4_cksum_fold(sum);
}

static int
ipv4_prefix_match(uint32_t addr,
                  uint32_t prefix,
                  int prefixlen)
{
  const uint32_t mask = htonl(mask_from_prefixlen(prefixlen));
  return (addr & mask) == (prefix & mask);
}







static struct nexthop_list ipv4_nexthops; // Make a hash

nexthop_t *
ipv4_nexthop_resolve(uint32_t addr)
{
  nexthop_t *nh;
  LIST_FOREACH(nh, &ipv4_nexthops, nh_global_link) {
    if(nh->nh_addr == addr)
      return nh;
  }

  netif_t *ni;
  SLIST_FOREACH(ni, &netifs, ni_global_link) {
    if(ni->ni_ipv4_local_addr == 0)
      continue;
    if(ipv4_prefix_match(addr, ni->ni_ipv4_local_addr,
                         ni->ni_ipv4_local_prefixlen))
      break;
  }

  if(ni == NULL)
    return NULL;

  nh = xalloc(sizeof(nexthop_t), 0, MEM_MAY_FAIL);
  if(nh == NULL)
    return NULL;

  nh->nh_addr = addr;
  LIST_INSERT_HEAD(&ipv4_nexthops, nh, nh_global_link);

  nh->nh_netif = ni;
  LIST_INSERT_HEAD(&ni->ni_nexthops, nh, nh_netif_link);

  nh->nh_pending = NULL;
  nh->nh_state = NEXTHOP_IDLE;
  nh->nh_in_use = 0;
  return nh;
}


void
icmp_input_icmp_echo(netif_t *ni, pbuf_t *pb, int icmp_offset)
{
  ipv4_header_t *ip = pbuf_data(pb, 0);

  nexthop_t *nh = ipv4_nexthop_resolve(ip->src_addr);
  if(nh == NULL) {
    pbuf_free(pb);
    return;
  }

  icmp_hdr_t *icmp = pbuf_data(pb, icmp_offset);

  ip->dst_addr = ip->src_addr;
  ip->src_addr = ni->ni_ipv4_local_addr;

  icmp->type = 0;

  // ni is now the output interface
  ni = nh->nh_netif;

  icmp->cksum = 0;
  if(!(ni->ni_flags & NETIF_F_TX_ICMP_CKSUM_OFFLOAD)) {
    icmp->cksum = ipv4_cksum_pbuf(0, pb, icmp_offset, INT32_MAX);
  }

  ip->cksum = 0;
  if(!(ni->ni_flags & NETIF_F_TX_IPV4_CKSUM_OFFLOAD)) {
    ip->cksum = ipv4_cksum_pbuf(0, pb, 0, sizeof(ipv4_header_t));
  }
  nh->nh_netif->ni_output_ipv4(ni, nh, pb);
}


pbuf_t *
ipv4_input_icmp(netif_t *ni, pbuf_t *pb, int icmp_offset)
{
  // No address configured yet, no ICMP activity
  if(ni->ni_ipv4_local_addr == 0)
    return pb;

  if(pb->pb_flags & PBUF_MCAST)
    return pb;

  if(ipv4_cksum_pbuf(0, pb, icmp_offset, INT32_MAX)) {
    return pb;
  }

  if(pbuf_pullup(pb, icmp_offset + 4)) {
    return pb;
  }
  const icmp_hdr_t *icmp = pbuf_data(pb, icmp_offset);
  switch(icmp->type) {
  case 8: // ICMP_ECHO
    icmp_input_icmp_echo(ni, pb, icmp_offset);
    return NULL;

  default:
    break;
  }
  return pb;
}




pbuf_t *
ipv4_input(netif_t *ni, pbuf_t *pb)
{
  if(pbuf_pullup(pb, sizeof(ipv4_header_t)))
    return pb;

  const ipv4_header_t *ip = pbuf_data(pb, 0);
  if(ip->ver_ihl != 0x45)
    return pb;

  if(ipv4_cksum_pbuf(0, pb, 0, 20)) {
    // IP header checksum error
    return pb;
  }

  if(ntohs(ip->fragment_info) & 0x3fff) {
    // No fragment support
    return pb;
  }

  // Make sure packet buffer length matches length in IP header
  uint16_t len = ntohs(ip->total_length);
  if(len > pb->pb_pktlen) {
    // Header says packet is larger than it is, drop packet
    return pb;
  }

  if(len < pb->pb_pktlen) {
    // Header says packet is smaller than it is, trim packet buffer
    pbuf_trim(pb, pb->pb_pktlen - len);
  }

#if 0
  printf("\tIPV4 %Id > %Id %d (%d)\n",
         ip->src_addr, ip->dst_addr,
         ip->proto, pb->pb_pktlen);
#endif

  if(ip->dst_addr == 0xffffffff) {
    pb->pb_flags |= PBUF_BCAST;
  } else if((ip->dst_addr & htonl(0xf0000000u)) == htonl(0xe0000000u)) {
    pb->pb_flags |= PBUF_MCAST;
  }

  switch(ip->proto) {
  case IPPROTO_ICMP:
    return ipv4_input_icmp(ni, pb, 20);
  case  IPPROTO_IGMP:
    return igmp_input_ipv4(ni, pb, 20);
  case IPPROTO_UDP:
    return udp_input_ipv4(ni, pb, 20);
  case IPPROTO_TCP:
    return tcp_input_ipv4(ni, pb, 20);
  default:
    return pb;
  }
}
