#include <mios/service.h>
#include <malloc.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include "net/pbuf.h"


typedef struct svc_chargen {
  void *sc_opaque;
  int sc_cnt;
  socket_t *sc_sock;

} svc_chargen_t;


static pbuf_t *
chargen_pull(void *opaque)
{
  svc_chargen_t *sc = opaque;

  if(sc->sc_cnt == 100) {
    sc->sc_sock->net->event(sc->sc_sock->net_opaque, SOCKET_EVENT_CLOSE);
    return NULL;
  }
  sc->sc_cnt++;
  pbuf_t *pb = pbuf_make(sc->sc_sock->preferred_offset, 0);
  if(pb != NULL) {
    int len = snprintf(pbuf_data(pb, 0), 20, "%d\n", sc->sc_cnt);
    pb->pb_pktlen = len;
    pb->pb_buflen = len;
  }
  return pb;
}


static void
chargen_close(void *opaque)
{
  free(opaque);
}



static const socket_app_fn_t chargen_fn = {
  .pull = chargen_pull,
  .close = chargen_close
};


static error_t
chargen_open(socket_t *s)
{
  svc_chargen_t *sc = xalloc(sizeof(svc_chargen_t), 0, MEM_MAY_FAIL);
  if(sc == NULL)
    return ERR_NO_MEMORY;
  sc->sc_cnt = 0;
  sc->sc_sock = s;
  s->app = &chargen_fn;
  s->app_opaque = sc;

  return 0;
}


SERVICE_DEF("chargen", 19, 19, SERVICE_TYPE_STREAM, chargen_open);
