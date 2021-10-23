#include <mios/dsig.h>

#include "net/pbuf.h"
#include "mbus_dsig.h"

#include <stdio.h> // REMOVE

struct pbuf *
mbus_dsig_input(struct mbus_netif *mni, struct pbuf *pb,
                uint8_t remote_addr)
{
  if(pb->pb_pktlen >= 3) {
    if((pb = pbuf_pullup(pb, pb->pb_pktlen)) == NULL)
      return pb;
    const uint8_t *d = pbuf_cdata(pb, 0);
    dsig_emit(d[1], d + 3, pb->pb_pktlen - 3, d[2], DSIG_EMIT_LOCAL);
  }
  return pb;
}
