#include <mios/rpc.h>

#include <mios/service.h>
#include <mios/task.h>
#include <mios/bytestream.h>
#include <mios/eventlog.h>

#include <assert.h>
#include <malloc.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include "net/pbuf.h"


typedef struct svc_rpc {
  pushpull_t *pp;
  pbuf_t *req;
  pbuf_t *resp;
  mutex_t mutex;
  cond_t cond;
  int stop;
} svc_rpc_t;


static int
rpc_may_push(void *opaque)
{
  svc_rpc_t *sr = opaque;
  return sr->req == NULL;
}

static uint32_t
rpc_push(void *opaque, struct pbuf *pb)
{
  svc_rpc_t *sr = opaque;

  mutex_lock(&sr->mutex);
  assert(sr->req == NULL);
  sr->req = pb;
  cond_signal(&sr->cond);
  mutex_unlock(&sr->mutex);
  return 0;
}


static pbuf_t *
rpc_pull(void *opaque)
{
  svc_rpc_t *sr = opaque;
  mutex_lock(&sr->mutex);
  pbuf_t *pb = sr->resp;
  sr->resp = NULL;
  cond_signal(&sr->cond);
  mutex_unlock(&sr->mutex);
  return pb;
}


static pbuf_t *
rpc_err(pbuf_t *pb, error_t err)
{
  pbuf_reset(pb, 0, 0);
  uint8_t *p = pbuf_append(pb, 5);
  p[0] = 'e';
  wr32_le(p + 1, err);
  return pb;
}


static pbuf_t *
rpc_dispatch(pbuf_t *pb)
{
  if(pbuf_pullup(pb, pb->pb_pktlen)) {
    /* Request larger than what fits in a single pbuf
       TODO: We could try to allocate memory and read into it
    */
    return rpc_err(pb, ERR_MTU_EXCEEDED);
  }

  uint8_t *req = pbuf_data(pb, 0);
  size_t len = pb->pb_pktlen;
  const uint8_t namelen = req[0];
  if(1 + namelen > len) {
    return rpc_err(pb, ERR_MALFORMED);
  }

  rpc_result_t rr;
  error_t err = rpc_dispatch_cbor(&rr, (const char *)req + 1, namelen,
                                  req + 1 + namelen, len - 1 - namelen);
  if(err)
    return rpc_err(pb, err);

  pbuf_reset(pb, 0, 0);

  uint8_t *p = pbuf_append(pb, 1);
  p[0] = rr.type | 0x20; // make lowercase
  const void *src;

  switch(rr.type) {
  case 'i':
  case 'f':
    src = &rr.i32;
    len = 4;
    break;
  case 's':
  case 'S':
    src = rr.data;
    len = strlen(rr.data);
    break;
  case 'b':
  case 'B':
    src = rr.data;
    len = rr.size;
    break;
  default:
    src = NULL;
    len = 0;
    break;
  }

  p = pbuf_append(pb, len);
  memcpy(p, src, len);

  if(rr.type == 's' || rr.type == 'b')
    free(rr.data);
  return pb;
}


#include <unistd.h>

__attribute__((noreturn))
static void *
rpc_thread(void *arg)
{
  svc_rpc_t *sr = arg;

  mutex_lock(&sr->mutex);
  while(!sr->stop) {

    pbuf_t *pb = sr->req;
    if(pb == NULL) {
      cond_wait(&sr->cond, &sr->mutex);
      continue;
    }
    sr->req = NULL;
    mutex_unlock(&sr->mutex);
    pb = rpc_dispatch(pb);
    mutex_lock(&sr->mutex);
    sr->resp = pb;
    sr->pp->net->event(sr->pp->net_opaque, PUSHPULL_EVENT_PULL);
    while(sr->resp && !sr->stop) {
      cond_wait(&sr->cond, &sr->mutex);
    }
  }
  mutex_unlock(&sr->mutex);
  sr->pp->net->event(sr->pp->net_opaque, PUSHPULL_EVENT_CLOSE);

  if(sr->req)
    pbuf_free(sr->req);
  if(sr->resp)
    pbuf_free(sr->resp);
  free(sr);
  thread_exit(NULL);
}


static void
rpc_close(void *opaque, const char *reason)
{
  svc_rpc_t *sr = opaque;

  mutex_lock(&sr->mutex);
  sr->stop = 1;
  cond_signal(&sr->cond);
  mutex_unlock(&sr->mutex);
}



static const pushpull_app_fn_t rpc_fn = {
  .push = rpc_push,
  .may_push = rpc_may_push,
  .pull = rpc_pull,
  .close = rpc_close
};

static error_t
rpc_open(pushpull_t *pp)
{
  svc_rpc_t *sr = xalloc(sizeof(svc_rpc_t), 0, MEM_MAY_FAIL | MEM_CLEAR);
  if(sr == NULL)
    return ERR_NO_MEMORY;

  pp->app_opaque = sr;
  pp->app = &rpc_fn;

  mutex_init(&sr->mutex, "rpc");
  cond_init(&sr->cond, "rpc");

  sr->pp = pp;
  if(!thread_create(rpc_thread, sr, 0, "rpc", TASK_DETACHED, 3)) {
    free(sr);
    return ERR_NO_MEMORY;
  }
  return 0;
}


SERVICE_DEF_PUSHPULL("rpc", 0, 1, rpc_open);
