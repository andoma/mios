#include "udp.h"

#include "net/pbuf.h"
#include "net/netif.h"
#include "net/net.h"

#include "ipv4.h"
#include "dhcpv4.h"


pbuf_t *
udp_input_ipv4(netif_t *ni, pbuf_t *pb, int udp_offset)
{
  const ipv4_header_t *ip = pbuf_data(pb, 0);
  uint32_t src_addr = ip->src_addr;
  const udp_hdr_t *udp = pbuf_data(pb, udp_offset);

#if 0
  printf("UDP %Id:%d > %Id:%d\n",
         ip->src_addr,
         ntohs(udp->src_port),
         ip->dst_addr,
         ntohs(udp->dst_port));
#endif


  uint16_t dst_port = ntohs(udp->dst_port);

  if(dst_port == 68) {
    return dhcpv4_input(ni, pbuf_drop(pb, udp_offset + 8), src_addr);
  }

  return pb;
}
