#include <mios/service.h>
#include <malloc.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include "net/pbuf.h"


typedef struct svc_chargen {
  void *sc_opaque;
  service_event_cb_t *sc_cb;
  int sc_cnt;
} svc_chargen_t;


static void *
chargen_open(void *opaque, service_event_cb_t *cb, size_t max_fragment_size)
{
  svc_chargen_t *sc = xalloc(sizeof(svc_chargen_t), 0, MEM_MAY_FAIL);
  if(sc == NULL)
    return NULL;
  sc->sc_cnt = 0;
  sc->sc_opaque = opaque;
  sc->sc_cb = cb;
  return sc;
}

static pbuf_t *
chargen_pull(void *opaque)
{
  svc_chargen_t *sc = opaque;

  if(sc->sc_cnt == 100) {
    sc->sc_cb(sc->sc_opaque, SERVICE_EVENT_CLOSE);
    return NULL;
  }
  sc->sc_cnt++;
  pbuf_t *pb = pbuf_make(0, 0);
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

SERVICE_DEF("chargen",
            chargen_open, NULL, NULL, chargen_pull, chargen_close);
