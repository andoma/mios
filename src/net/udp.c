#include "pbuf.h"
#include "netif.h"
#include "ipv4.h"
#include "udp.h"
#include "net.h"
#include "dhcpv4.h"
#include "socket.h"

static struct socket_list udp_sockets; // FIXME: Make a hash

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

  socket_t *s;
  LIST_FOREACH(s, &udp_sockets, s_net_link) {
    if(s->s_local_port == dst_port) {
      return s->s_rx(s, pb);
    }
  }

  if(dst_port == 68) {
    return dhcpv4_input(ni, pbuf_drop(pb, udp_offset + 8), src_addr);
  }

  return pb;
}


error_t
udp_socket_attach(struct socket *s)
{
  LIST_INSERT_HEAD(&udp_sockets, s, s_net_link);
  return 0;
}

error_t
udp_socket_detach(struct socket *s)
{
  LIST_REMOVE(s, s_net_link);
  return 0;
}
