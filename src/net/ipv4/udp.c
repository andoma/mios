#include "udp.h"

#include "net/pbuf.h"
#include "net/netif.h"
#include "net/net.h"

#include "ipv4.h"
#include "dhcpv4.h"

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
