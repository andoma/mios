#include "mbus_rpc.h"

#include "net/pbuf.h"
#include "net/mbus/mbus.h"
#include "net/mbus/mbus_flow.h"

#include <mios/error.h>
#include <mios/rpc.h>


static pbuf_t *
mbus_rpc_err(struct pbuf *pb, const mbus_flow_t *mf, error_t code)
{
  pbuf_reset(pb, 4, 0);
  pb = pbuf_prepend(pb, 1);
  if(pb == NULL)
    return pb;
  uint8_t *pkt = pbuf_data(pb, 0);
  pkt[0] = code;
  return mbus_output_flow(pb, mf);
}


struct pbuf *
mbus_rpc_dispatch(struct pbuf *pb, uint8_t src_addr, uint16_t flow)
{
  mbus_flow_t mf = {.mf_flow = flow, .mf_remote_addr = src_addr};

  if(pb->pb_buflen < 1)
    return pb;

  const uint8_t *pkt = pbuf_cdata(pb, 0);
  const uint8_t namelen = pkt[0];
  if(pb->pb_buflen < 1 + namelen)
    return pb;

  const rpc_method_t *rm = rpc_method_resovle(pkt + 1, namelen);
  if(rm == NULL)
    return mbus_rpc_err(pb, &mf, ERR_INVALID_RPC_ID);

  const int skip = (namelen + 1 + 3) & ~3;
  if(skip > pb->pb_pktlen) {
    return mbus_rpc_err(pb, &mf, ERR_MALFORMED);
  }

  pkt += skip;
  size_t len = pb->pb_pktlen - skip;

  pbuf_t *reply = pbuf_make(4, 0);
  if(reply == NULL)
    return mbus_rpc_err(pb, &mf, ERR_NO_BUFFER);

  error_t err = rm->invoke(pkt, pbuf_data(reply, 0), len);
  if(err) {
    pbuf_free(reply);
    return mbus_rpc_err(pb, &mf, ERR_NO_BUFFER);
  }
  pbuf_free(pb);
  reply = pbuf_prepend(reply, 1);
  if(reply == NULL)
    return NULL;
  uint8_t *out = pbuf_data(reply, 0);
  out[0] = 0;
  return mbus_output_flow(reply, &mf);
}
