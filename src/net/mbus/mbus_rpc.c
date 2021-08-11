#include "mbus_rpc.h"
#include "mbus.h"

#include <mios/version.h>
#include <mios/rpc.h>

#include <string.h>
#include <stdio.h>

#include "net/pbuf.h"

extern unsigned long _rpcdef_array_begin;
extern unsigned long _rpcdef_array_end;


static uint32_t rpc_method_salt;


struct pbuf *
mbus_handle_rpc_resolve(struct mbus_netif *mni, struct pbuf *pb,
                        uint8_t remote_addr)
{
  if(pb->pb_pktlen < 3)
    return pb;

  uint8_t *pkt = pbuf_data(pb, 0);
  pkt[0] = MBUS_OP_RPC_RESOLVE_REPLY;

  const uint8_t *req_name = pkt + 2;
  const size_t req_namelen = pb->pb_pktlen - 2;

  const rpc_method_t *rm = (void *)&_rpcdef_array_begin;
  uint32_t id = rpc_method_salt;
  for(; rm != (const void *)&_rpcdef_array_end; rm++, id++) {
    const size_t len = strlen(rm->name);
    if(req_namelen == len && !memcmp(rm->name, req_name, len)) {
      pb = pbuf_trim(pb, req_namelen);
      uint32_t *u32p = pbuf_append(pb, sizeof(uint32_t));
      *u32p = id;
      mbus_output(mni, pb, remote_addr);
      return NULL;
    }
  }
  pb = pbuf_trim(pb, req_namelen);
  mbus_output(mni, pb, remote_addr);
  return NULL;
}


struct pbuf *
mbus_rpc_error(struct mbus_netif *mni, struct pbuf *pb,
               uint8_t remote_addr, error_t err)
{
  pb = pbuf_trim(pb, pb->pb_pktlen - 2);
  uint8_t *pkt = pbuf_data(pb, 0);
  pkt[0] = MBUS_OP_RPC_ERR;
  int *errptr = pbuf_append(pb, sizeof(int32_t));
  *errptr = err;
  mbus_output(mni, pb, remote_addr);
  return NULL;
}


struct pbuf *
mbus_handle_rpc_invoke(struct mbus_netif *mni, struct pbuf *pb,
                       uint8_t remote_addr)
{
  if(pb->pb_pktlen < 6)
    return pb;

  uint8_t *pkt = pbuf_data(pb, 0);

  uint32_t method_id;
  memcpy(&method_id, pkt + 2, sizeof(uint32_t));

  const rpc_method_t *rpc_mbase = (void *)&_rpcdef_array_begin;
  const rpc_method_t *rpc_mend = (void *)&_rpcdef_array_end;
  const uint32_t num_methods = rpc_mend - rpc_mbase;

  method_id -= rpc_method_salt;
  if(method_id >= num_methods)
    return mbus_rpc_error(mni, pb, remote_addr, ERR_INVALID_RPC_ID);

  const void *in = pkt + 6;
  const size_t in_len = pb->pb_pktlen - 6;

  const rpc_method_t *m = rpc_mbase + method_id;

  if(m->in_size != 0xffff && m->in_size != in_len) {
    return mbus_rpc_error(mni, pb, remote_addr, ERR_INVALID_RPC_ARGS);
  }

  uint8_t reply[m->out_size]; // FIXME: Avoid VLA
  error_t err = m->invoke(in, reply, in_len);
  if(err)
    return mbus_rpc_error(mni, pb, remote_addr, err);

  pb = pbuf_trim(pb, pb->pb_pktlen - 2);
  pkt = pbuf_data(pb, 0);
  pkt[0] = MBUS_OP_RPC_REPLY;
  void *outptr = pbuf_append(pb, m->out_size);
  memcpy(outptr, reply, m->out_size);
  mbus_output(mni, pb, remote_addr);
  return NULL;
}


static void __attribute__((constructor(200)))
mbus_rpc_init(void)
{
  memcpy(&rpc_method_salt, mios_build_id(), sizeof(rpc_method_salt));
}
