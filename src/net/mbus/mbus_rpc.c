#include "mbus_rpc.h"
#include "mbus.h"

#include <mios/version.h>
#include <mios/rpc.h>

#include <string.h>
#include <stdio.h>
#include <unistd.h>

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

static uint32_t
rd32_le(const uint8_t *ptr)
{
  return ptr[0] | (ptr[1] << 8) | (ptr[2] << 16) | (ptr[3] << 24);
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
  pbuf_reset(pb, MBUS_HDR_LEN, 6);
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

static error_t
rpc_appname(const void *in, void *out, size_t in_size)
{
  const char *appname = mios_get_app_name();
  strlcpy(out, appname, 24);
  return 0;
}

RPC_DEF("appname", 0, 24, rpc_appname, 0);




typedef struct mbus_pending_rpc {
  LIST_ENTRY(mbus_pending_rpc) mpr_link;
  pbuf_t *mpr_pbuf;
  uint32_t mpr_key;
} mbus_pending_rpc_t;

static LIST_HEAD(, mbus_pending_rpc) mbus_pending_rpcs;

typedef struct {
  const char *method;
  uint32_t id;
  uint8_t target;
} mbus_rpc_cache_entry_t;

//#define MBUS_RPC_CACHE_SIZE 4
//static mbus_rpc_cache_entry_t mbus_rpc_cache[MBUS_RPC_CACHE_SIZE];


static mutex_t mbus_rpc_mutex = MUTEX_INITIALIZER("rpc");
static cond_t mbus_rpc_cond = COND_INITIALIZER("rpc");

static uint8_t mbus_rpc_txid;

static pbuf_t *
mbus_wakeup(uint32_t key, pbuf_t *pb)
{
  if(pb->pb_pktlen < 2)
    return pb;

  const uint8_t *u8 = pbuf_data(pb, 0);
  const uint8_t txid = u8[1];
  key |= txid;

  mutex_lock(&mbus_rpc_mutex);
  mbus_pending_rpc_t *mpr;
  LIST_FOREACH(mpr, &mbus_pending_rpcs, mpr_link) {
    if(mpr->mpr_key == key) {
      mpr->mpr_pbuf = pb;
      pb = NULL;
      cond_broadcast(&mbus_rpc_cond);
    }
  }
  mutex_unlock(&mbus_rpc_mutex);
  return pb;
}


struct pbuf *
mbus_handle_rpc_response(struct mbus_netif *mni,
                         struct pbuf *pb,
                         uint8_t remote_addr)
{
  return mbus_wakeup((remote_addr << 8), pb);
}

static pbuf_t *
mbus_rpc_xmit(uint8_t remote_addr, pbuf_t *pb)
{
  uint8_t *u8 = pbuf_data(pb, 0);
  mbus_pending_rpc_t mpr;
  mpr.mpr_pbuf = NULL;
  mutex_lock(&mbus_rpc_mutex);
  uint8_t txid = ++mbus_rpc_txid;

  mpr.mpr_key = (remote_addr << 8) | txid;
  LIST_INSERT_HEAD(&mbus_pending_rpcs, &mpr, mpr_link);
  u8[1] = mbus_rpc_txid++;
  pbuf_free(mbus_xmit(remote_addr, pb));

  uint64_t deadline = clock_get() + 250000;

  while(mpr.mpr_pbuf == NULL) {
    if(cond_wait_timeout(&mbus_rpc_cond, &mbus_rpc_mutex, deadline)) {
      // Timeout
      break;
    }
  }
  LIST_REMOVE(&mpr, mpr_link);
  mutex_unlock(&mbus_rpc_mutex);
  return mpr.mpr_pbuf;
}


static pbuf_t *
resolve_rpc_id(uint8_t remote_addr, const char *method, error_t *errp)
{
  pbuf_t *pb = pbuf_make(MBUS_HDR_LEN, 1);
  size_t mlen = strlen(method);

  uint8_t *outdata = pbuf_append(pb, 2 + mlen);
  outdata[0] = MBUS_OP_RPC_RESOLVE;
  memcpy(outdata + 2, method, mlen);

  pb = mbus_rpc_xmit(remote_addr, pb);
  if(pb == NULL) {
    *errp = ERR_TIMEOUT;
    return pb;
  }

  if(pb->pb_pktlen < 6) {
    *errp = ERR_INVALID_RPC_ID;
    pbuf_free(pb);
    return NULL;
  }
  return pb;
}


pbuf_t *
mbus_rpc(uint8_t remote_addr, const char *method, const uint8_t *data,
         size_t len, error_t *errp)
{
  error_t err;
  if(errp == NULL)
    errp = &err;

  pbuf_t *pb = resolve_rpc_id(remote_addr, method, errp);
  if(pb == NULL)
    return NULL;

  void *reqdata = pbuf_append(pb, len);
  memcpy(reqdata, data, len);
  uint8_t *d8 = pbuf_data(pb, 0);
  d8[0] = MBUS_OP_RPC_INVOKE;
  pb = mbus_rpc_xmit(remote_addr, pb);
  if(pb == NULL) {
    *errp = ERR_TIMEOUT;
    return NULL;
  }

  const uint8_t *u8 = pbuf_data(pb, 0);
  if(u8[0] == MBUS_OP_RPC_ERR) {
    if(pb->pb_pktlen < 6) {
      *errp = ERR_MALFORMED;
    } else {
      *errp = rd32_le(u8 + 2);
    }
    pbuf_free(pb);
    return NULL;
  }
  return pbuf_drop(pb, 2);
}
