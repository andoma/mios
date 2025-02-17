#include "can.h"

#include <assert.h>

#include <mios/bytestream.h>
#include <mios/dsig.h>

#include "net/dsig.h"

struct pbuf *
can_input(struct netif *ni, struct pbuf *pb)
{
  if(pb->pb_pktlen < 4)
    return pb;

  if(pbuf_pullup(pb, pb->pb_pktlen))
    return pb;

  const void *data = pbuf_cdata(pb, 0);
  uint32_t signal = rd32_le(data);

  pb = pbuf_drop(pb, 4);

  return dsig_input(signal, pb, ni);
}


static pbuf_t *
can_dsig_output(struct netif *ni, pbuf_t *pb, uint32_t id, uint32_t flags)
{
  can_netif_t *cni = (can_netif_t *)ni;
  return cni->cni_output(cni, pb, id);
}


void
can_netif_attach(can_netif_t *cni, const char *name,
                 const device_class_t *dc,
                 const struct dsig_filter *output_filter)
{
  cni->cni_ni.ni_dsig_output = can_dsig_output;
  cni->cni_ni.ni_dsig_output_filter = output_filter;

  cni->cni_ni.ni_input = can_input;
  cni->cni_ni.ni_mtu = 8; // Should be set by caller

  netif_attach(&cni->cni_ni, name, dc);
}
