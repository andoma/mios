#if 0

#include "udp.h"

#include "net/pbuf.h"
#include "net/netif.h"
#include "net/net.h"

#include "ipv4.h"
#include "dhcpv4.h"

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
  LIST_FOREACH(s, &udp_sockets, s_proto_link) {
    if(s->s_local_port == dst_port) {
      if((pb = pbuf_drop(pb, udp_offset + 8)) == NULL)
        return NULL;
      return s->s_rx(s, pb);
    }
  }

  if(dst_port == 68) {
    return dhcpv4_input(ni, pbuf_drop(pb, udp_offset + 8), src_addr);
  }

  return pb;
}


static error_t
udp_control(socket_t *s, socket_ctl_t *sc)
{
  switch(sc->sc_op) {
  case SOCKET_CTL_ATTACH:
    LIST_INSERT_HEAD(&udp_sockets, s, s_proto_link);
    return 0;
  case SOCKET_CTL_DETACH:
    LIST_REMOVE(s, s_proto_link);
    return 0;
  }
  return ERR_NOT_IMPLEMENTED;
}


static pbuf_t *
udp_send(socket_t *s, pbuf_t *pb)
{
  return pb;
}

NET_SOCKET_PROTO_DEF(AF_INET, IPPROTO_UDP, udp_control, udp_send);
#endif
