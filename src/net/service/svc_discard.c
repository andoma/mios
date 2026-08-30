#include <mios/service.h>

#include "net/pbuf.h"

static uint32_t
discard_push(void *opaque, struct pbuf *pb)
{
  pbuf_free(pb);
  return 0;
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
  // Nothing to free, no state, no need to tell the network side anything
  // -- it already knows it's closing (that's why it called us).
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
  s->app_opaque = NULL;
  return 0;
}

SERVICE_DEF_PUSHPULL("discard", 9, 9, discard_open);
