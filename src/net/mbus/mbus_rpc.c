#include "mbus_rpc.h"
#include "mbus.h"

#include <mios/version.h>
#include <mios/rpc.h>

#include <string.h>
#include <stdio.h>

#include "irq.h"
#include "net/pbuf.h"

extern unsigned long _rpcdef_array_begin;
extern unsigned long _rpcdef_array_end;


static uint32_t rpc_method_salt;


static void
wr32_le(uint8_t *ptr, uint32_t u32)
{
  ptr[0] = u32;
  ptr[1] = u32 >> 8;
  ptr[2] = u32 >> 16;
  ptr[3] = u32 >> 24;
}


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
      uint8_t *u8p = pbuf_append(pb, sizeof(uint32_t));
      wr32_le(u8p, id);
      return mbus_output(mni, pb, remote_addr);
    }
  }
  pb = pbuf_trim(pb, req_namelen);
  return mbus_output(mni, pb, remote_addr);
}


struct pbuf *
mbus_rpc_error(struct mbus_netif *mni, struct pbuf *pb,
               uint8_t remote_addr, error_t err)
{
  const uint8_t txid = *(const uint8_t *)pbuf_cdata(pb, 1);
  pbuf_reset(pb, 0, 6);
  uint8_t *pkt = pbuf_data(pb, 0);
  pkt[0] = MBUS_OP_RPC_ERR;
  pkt[1] = txid;
  wr32_le(pkt + 2, err);
  return mbus_output(mni, pb, remote_addr);
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

  int q = irq_forbid(IRQ_LEVEL_NET);
  void *pbc = pbuf_data_get(1);
  irq_permit(q);

  memcpy(pbc, in, in_len);

  void *reply = pb->pb_data + 8;

  error_t err = m->invoke(pbc, reply, in_len);

  q = irq_forbid(IRQ_LEVEL_NET);
  pbuf_data_put(pbc);
  irq_permit(q);

  if(err)
    return mbus_rpc_error(mni, pb, remote_addr, err);

  const uint8_t txid = *(const uint8_t *)pbuf_cdata(pb, 1);
  pbuf_reset(pb, 6, 2 + m->out_size);
  pkt = pbuf_data(pb, 0);
  pkt[0] = MBUS_OP_RPC_REPLY;
  pkt[1] = txid;
  return mbus_output(mni, pb, remote_addr);
}


static void __attribute__((constructor(200)))
mbus_rpc_init(void)
{
  memcpy(&rpc_method_salt, mios_build_id(), sizeof(rpc_method_salt));
}


static error_t
rpc_ping(const void *in, void *out, size_t in_size)
{
  return 0;
}

RPC_DEF("ping", 0, 0, rpc_ping, 0);

static error_t
rpc_buildid(const void *in, void *out, size_t in_size)
{
  memcpy(out, mios_build_id(), 20);
  return 0;
}

RPC_DEF("buildid", 0, 20, rpc_buildid, 0);
