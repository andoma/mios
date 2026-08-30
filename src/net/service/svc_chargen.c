#include <mios/service.h>
#include <malloc.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include "net/pbuf.h"


typedef struct svc_chargen {
  pushpull_t *sc_sock;
} svc_chargen_t;


// Runs at full speed until the peer closes the channel (classic RFC 864
// semantics) -- a throughput/reliability test, not a content test, so
// each packet is filled to the transport's own max fragment size with a
// cheap, position-derived byte pattern rather than small formatted text.
static pbuf_t *
chargen_pull(void *opaque)
{
  svc_chargen_t *sc = opaque;
  pushpull_t *s = sc->sc_sock;

  pbuf_t *pb = pbuf_make(s->preferred_offset, 0);
  if(pb == NULL)
    return NULL;

  const size_t len = s->max_fragment_size;
  uint8_t *data = pbuf_data(pb, 0);
  for(size_t i = 0; i < len; i++)
    data[i] = (uint8_t)i;
  pb->pb_pktlen = len;
  pb->pb_buflen = len;
  return pb;
}


static void
chargen_close(void *opaque, const char *reason)
{
  free(opaque);
}


static const pushpull_app_fn_t chargen_fn = {
  .pull = chargen_pull,
  .close = chargen_close
};


static error_t
chargen_open(pushpull_t *s)
{
  svc_chargen_t *sc = xalloc(sizeof(svc_chargen_t), 0, MEM_MAY_FAIL);
  if(sc == NULL)
    return ERR_NO_MEMORY;
  sc->sc_sock = s;
  s->app = &chargen_fn;
  s->app_opaque = sc;

  return 0;
}


SERVICE_DEF_PUSHPULL("chargen", 19, 19, chargen_open);
