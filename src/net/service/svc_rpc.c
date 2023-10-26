#if 0

#include <mios/rpc.h>

#include <mios/service.h>
#include <mios/version.h>

#include <assert.h>
#include <malloc.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include "net/pbuf.h"


typedef struct svc_rpc {
  //  void *sr_opaque;
  //  service_event_cb_t *sr_cb;
  pbuf_t *sr_pb;
  //  svc_pbuf_policy_t sr_pbuf_policy;
} svc_rpc_t;


static void *
rpc_open(void *opaque, service_event_cb_t *cb,
         svc_pbuf_policy_t pbuf_policy,
         service_get_flow_header_t *get_flow_hdr)
{
  svc_rpc_t *sr = xalloc(sizeof(svc_rpc_t), 0, MEM_MAY_FAIL);
  if(sr == NULL)
    return NULL;
  sr->sr_opaque = opaque;
  sr->sr_cb = cb;
  sr->sr_pb = NULL;
  sr->sr_pbuf_policy = pbuf_policy;
  return sr;
}


const rpc_method_t *
rpc_method_resovle(const uint8_t *req_name, size_t req_len)
{
  extern unsigned long _rpcdef_array_begin;
  extern unsigned long _rpcdef_array_end;

  const rpc_method_t *rm = (void *)&_rpcdef_array_begin;
  for(; rm != (const void *)&_rpcdef_array_end; rm++) {
    const size_t len = strlen(rm->name);
    if(req_len == len && !memcmp(rm->name, req_name, len)) {
      return rm;
    }
  }
  return NULL;
}


static int
rpc_may_push(void *opaque)
{
  svc_rpc_t *sr = opaque;
  return sr->sr_pb == NULL;
}

static pbuf_t *
rpc_pull(void *opaque)
{
  svc_rpc_t *sr = opaque;
  pbuf_t *pb = sr->sr_pb;
  sr->sr_pb = NULL;
  return pb;
}

static void
rpc_tx(svc_rpc_t *sr, pbuf_t *pb)
{
  assert(sr->sr_pb == NULL);
  sr->sr_pb = pb;
  sr->sr_cb(sr->sr_opaque, SERVICE_EVENT_WAKEUP);
}



static pbuf_t *
rpc_error(svc_rpc_t *sr, pbuf_t *pb, uint32_t err)
{
  pbuf_reset(pb, 0, 0);
  memcpy(pbuf_append(pb, sizeof(err)), &err, sizeof(err));
  rpc_tx(sr, pb);
  return NULL;
}


static pbuf_t *
rpc_push(void *opaque, struct pbuf *pb)
{
  svc_rpc_t *sr = opaque;

  if(pb->pb_pktlen < 1) {
    return rpc_error(sr, pb, ERR_MALFORMED);
  }

  if(pbuf_pullup(pb, pb->pb_pktlen)) {
    return rpc_error(sr, pb, ERR_MALFORMED);
  }

  const uint8_t *pkt = pbuf_cdata(pb, 0);
  const uint8_t namelen = pkt[0];
  if(1 + namelen > pb->pb_pktlen) {
    return rpc_error(sr, pb, ERR_MALFORMED);
  }

  const rpc_method_t *rm = rpc_method_resovle(pkt + 1, namelen);
  if(rm == NULL) {
    return rpc_error(sr, pb, ERR_INVALID_RPC_ID);
  }

  const int arg_offset = (1 + namelen + 3) & ~3;
  if(arg_offset > pb->pb_pktlen) {
    return rpc_error(sr, pb, ERR_INVALID_RPC_ID);
  }

  pb = pbuf_drop(pb, arg_offset);

  size_t in_len = pb->pb_pktlen;
  if(rm->in_size != 0xffff && rm->in_size != in_len) {
    rpc_error(sr, pb, ERR_INVALID_RPC_ARGS);
  }

  pbuf_t *resp = pbuf_make(sr->sr_pbuf_policy.preferred_offset, 0);
  if(resp == NULL) {
    return rpc_error(sr, pb, ERR_NO_BUFFER);
  }

  pbuf_reset(resp, 0, 4 + rm->out_size);

  uint32_t err = rm->invoke(pbuf_cdata(pb, 0), pbuf_data(resp, 4), in_len);

  if(err) {
    pbuf_free(resp);
    return rpc_error(sr, pb, err);
  }

  memcpy(pbuf_data(resp, 0), &err, 4);

  rpc_tx(sr, resp);
  return pb;
}


static void
rpc_close(void *opaque)
{
  svc_rpc_t *sr = opaque;
  sr->sr_cb(sr->sr_opaque, SERVICE_EVENT_CLOSE);
  pbuf_free(sr->sr_pb);
  free(sr);
}

SERVICE_DEF("rpc", 0, 1, SERVICE_TYPE_DGRAM,
            rpc_open, rpc_push, rpc_may_push, rpc_pull, rpc_close);

#endif
