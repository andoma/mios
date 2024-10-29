#include "udp.h"

#include "net/pbuf.h"
#include "net/netif.h"
#include "net/net.h"

#include "ipv4.h"
#include "dhcpv4.h"

#include <stdlib.h>

pbuf_t *
udp_input_ipv4(netif_t *ni, pbuf_t *pb, size_t udp_offset)
{
  const udp_hdr_t *udp = pbuf_data(pb, udp_offset);

  const uint16_t dst_port = ntohs(udp->dst_port);

  extern unsigned long _udpinput_array_begin;
  extern unsigned long _udpinput_array_end;

  const udp_input_t *ui = (void *)&_udpinput_array_begin;
  for(; ui != (const void *)&_udpinput_array_end; ui++) {
    if(ui->port == dst_port) {
      return ui->input(ni, pb, udp_offset);
    }
  }
  return pb;
}


void
udp_send(netif_t *ni, pbuf_t *pb, uint32_t dst_addr, nexthop_t *nh,
         int src_port, int dst_port)
{
  if(ni == NULL) {
    nh = ipv4_nexthop_resolve(dst_addr);
    if(nh == NULL) {
      pbuf_free(pb);
      return;
    }
    ni = nh->nh_netif;
  }

  pb = pbuf_prepend(pb, sizeof(udp_hdr_t), 1, sizeof(ipv4_header_t));

  udp_hdr_t *udp = pbuf_data(pb, 0);
  udp->dst_port = htons(dst_port);
  udp->src_port = htons(src_port);
  udp->length = htons(pb->pb_pktlen);

  udp->cksum = 0;

  if(!(ni->ni_flags & NETIF_F_TX_IPV4_CKSUM_OFFLOAD)) {
    udp->cksum =
      ipv4_cksum_pbuf(ipv4_cksum_pseudo(ni->ni_ipv4_local_addr, dst_addr,
                                        IPPROTO_UDP, pb->pb_pktlen),
                      pb, 0, pb->pb_pktlen);
  }

  pb = pbuf_prepend(pb, sizeof(ipv4_header_t), 1, 0);
  ipv4_header_t *ip = pbuf_data(pb, 0);

  ip->ver_ihl = 0x45;
  ip->tos = 0;
  ip->total_length = htons(pb->pb_pktlen);
  ip->id = rand();
  ip->fragment_info = 0;
  ip->ttl = 255;
  ip->proto = IPPROTO_UDP;
  ip->src_addr = ni->ni_ipv4_local_addr;
  ip->dst_addr = dst_addr;

  ip->cksum = 0;
  if(!(ni->ni_flags & NETIF_F_TX_IPV4_CKSUM_OFFLOAD)) {
    ip->cksum = ipv4_cksum_pbuf(0, pb, 0, sizeof(ipv4_header_t));
  }
  ni->ni_output_ipv4(ni, nh, pb);
}
