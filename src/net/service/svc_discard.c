#include <mios/service.h>
#include <malloc.h>
#include <stdlib.h>
#include <assert.h>
#include <string.h>
#include <mios/eventlog.h>

#include "net/pbuf.h"


static pbuf_t *
discard_push(void *opaque, struct pbuf *pb)
{
  int id = -1;
  if(pb->pb_buflen >= 4)
    memcpy(&id, pbuf_cdata(pb, 0), 4);
  //  evlog(LOG_DEBUG, "Disc-0x%x %d bytes", id, pb->pb_pktlen);
  return pb;
}

static int
discard_may_push(void *opaque)
{
  return 1;
}


static pbuf_t *
discard_pull(void *opaque)
{
  return NULL;
}


static void
discard_close(void *opaque)
{
  socket_t *s = opaque;
  s->net->event(s->net_opaque, SOCKET_EVENT_CLOSE);
}

static const socket_app_fn_t discard_fn = {
  .push = discard_push,
  .may_push = discard_may_push,
  .pull = discard_pull,
  .close = discard_close
};

static error_t
discard_open(socket_t *s)
{
  s->app = &discard_fn;
  s->app_opaque = s;
  return 0;
}

SERVICE_DEF("discard", 9, 9, SERVICE_TYPE_DGRAM, discard_open);
