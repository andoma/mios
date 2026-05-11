#include "dsig_udp.h"

#include <mios/bytestream.h>

#include "net.h"
#include "netif.h"
#include "pbuf.h"
#include "dsig.h"
#include "ipv4/ipv4.h"
#include "ipv4/udp.h"

#ifndef DSIG_UDP_PORT
#define DSIG_UDP_PORT 0xd516
#endif

#ifndef DSIG_UDP_GROUP
#define DSIG_UDP_GROUP htonl(0xefffd516u) // 239.255.213.22
#endif

static pbuf_t *
dsig_udp_input(struct netif *ni, pbuf_t *pb, size_t udp_offset)
{
  pb = pbuf_drop(pb, udp_offset + sizeof(udp_hdr_t), 0);
  if(pb == NULL)
    return NULL;
  if(pb->pb_pktlen < 4)
    return pb;
  if(pbuf_pullup(pb, pb->pb_pktlen))
    return pb;

  uint32_t signal = rd32_le(pbuf_cdata(pb, 0));
  pb = pbuf_drop(pb, 4, 0);
  return dsig_input(signal, pb, ni);
}

UDP_INPUT(dsig_udp_input, DSIG_UDP_PORT);


pbuf_t *
dsig_udp_output(struct netif *ni, pbuf_t *pb, uint32_t id, uint32_t flags)
{
  pb = pbuf_prepend(pb, 4, 1, sizeof(ipv4_header_t) + sizeof(udp_hdr_t));
  if(pb == NULL)
    return NULL;
  wr32_le(pbuf_data(pb, 0), id);
  udp_send(ni, pb, DSIG_UDP_GROUP, NULL, DSIG_UDP_PORT, DSIG_UDP_PORT);
  return NULL;
}
