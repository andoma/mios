
#include <mios/service.h>
#include <malloc.h>
#include <stdlib.h>
#include <assert.h>
#include <string.h>
#include <mios/eventlog.h>

#if 0
#include "net/pbuf.h"


static uint32_t
discard_push(void *opaque, struct pbuf *pb)
{
  int id = -1;
  if(pb->pb_buflen >= 4)
    memcpy(&id, pbuf_cdata(pb, 0), 4);
  //  evlog(LOG_DEBUG, "Disc-0x%x %d bytes", id, pb->pb_pktlen);
  pbuf_free(pb);
  return PUSHPULL_EVENT_PUSH;
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
discard_close(void *opaque, const char *reason)
{
  pushpull_t *s = opaque;
  s->net->event(s->net_opaque, PUSHPULL_EVENT_CLOSE);
}

static const pushpull_app_fn_t discard_fn = {
  .push = discard_push,
  .may_push = discard_may_push,
  .pull = discard_pull,
  .close = discard_close
};

static error_t
discard_open(pushpull_t *s)
{
  s->app = &discard_fn;
  s->app_opaque = s;
  return 0;
}

SERVICE_DEF_PUSHPULL("discard", 9, 9, discard_open);
#endif
