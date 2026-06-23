#include <mios/sigcapture.h>
#include <mios/service.h>
#include <mios/task.h>

#include <string.h>

#include <net/pbuf.h>

#include "irq.h"

#include "sigcapture_internal.h"

// Streams sigcapture traces over the VLLP "sigcapture" pushpull service
// (CAN/dsig). Useful where USB is unavailable, and for pulling a frozen trace
// after a fault in production builds.
//
// Concurrency model (see sigcapture.c for the ring-buffer invariant):
//   - sigcapture_wrptr() fills the ring only in state RUN and freezes it in
//     READOUT, so the buffer is read-only to the reader during a readout.
//   - On completion the sigcapture softirq calls sc_vllp_on_complete() (at
//     PendSV / IRQ_LEVEL_SWITCH); it does nothing heavier than
//     pushpull_wakeup(), which is safe from that level.
//   - sc_vllp_pull()/sc_vllp_push() run in net context (IRQ_LEVEL_NET) and are
//     the only place pbufs are allocated and frames serialized. Because the
//     buffer is frozen during readout, no lock is needed.

typedef struct sc_vllp {
  sigcapture_t *sc;
  pushpull_t *pp;  // bound channel, or NULL when no host is connected
} sc_vllp_t;

// Single owner: at most one VLLP sigcapture instance / one bound channel.
static sc_vllp_t g_scv;


// Completion hook, invoked from the sigcapture softirq (PendSV).
static void
sc_vllp_on_complete(void *opaque)
{
  sc_vllp_t *scv = opaque;
  if(scv->pp == NULL)
    return;  // No host: leave the capture frozen in READOUT until one connects.

  scv->sc->send_index = 0;  // Start the readout cursor.
  pushpull_wakeup(scv->pp, PUSHPULL_EVENT_PULL);
}


static uint32_t
sc_vllp_push(void *opaque, pbuf_t *pb)
{
  sc_vllp_t *scv = opaque;

  // Optional little-endian uint16 leading-sample count; 0 (or absent) means
  // the sigcapture default (half the buffer before the trigger).
  size_t leading = 0;
  if(pb->pb_buflen >= 2) {
    const uint8_t *u8 = pbuf_cdata(pb, 0);
    leading = u8[0] | (u8[1] << 8);
  }

  sigcapture_trig(scv->sc, leading);
  pbuf_free(pb);
  return 0;
}


static int
sc_vllp_may_push(void *opaque)
{
  return 1;  // sigcapture_trig() itself ignores triggers while busy.
}


static pbuf_t *
sc_vllp_pull(void *opaque)
{
  sc_vllp_t *scv = opaque;
  sigcapture_t *sc = scv->sc;

  if(sc->state != SIGCAPTURE_STATE_READOUT)
    return NULL;

  // Allocate before advancing the cursor: if the pool is momentarily empty we
  // return NULL without losing a frame, and the vllp engine re-polls us on the
  // next ACK / activity.
  pbuf_t *pb = pbuf_make(scv->pp->preferred_offset, 0);
  if(pb == NULL)
    return NULL;

  const void *data;
  size_t len;
  if(!sigcapture_readout_next(sc, &data, &len)) {
    // Readout complete; buffer already handed back to the sampler.
    pbuf_free(pb);
    return NULL;
  }

  void *dst = pbuf_append(pb, len);
  memcpy(dst, data, len);
  return pb;
}


static void
sc_vllp_close(void *opaque, const char *reason)
{
  sc_vllp_t *scv = opaque;

  // If the host drops mid-readout, discard the partial trace and let
  // sampling resume.
  if(scv->sc->state == SIGCAPTURE_STATE_READOUT)
    sigcapture_reset(scv->sc);

  scv->pp = NULL;
}


static const pushpull_app_fn_t sc_vllp_fn = {
  .push = sc_vllp_push,
  .may_push = sc_vllp_may_push,
  .pull = sc_vllp_pull,
  .close = sc_vllp_close,
};


static error_t
sc_vllp_open(pushpull_t *pp)
{
  sc_vllp_t *scv = &g_scv;

  if(scv->sc == NULL)
    return ERR_NOT_READY;  // sigcapture_create_vllp() not called yet.

  int q = irq_forbid(IRQ_LEVEL_SWITCH);
  if(scv->pp != NULL) {
    irq_permit(q);
    return ERR_NOT_READY;  // Single owner: a channel is already bound.
  }
  pp->app = &sc_vllp_fn;
  pp->app_opaque = scv;
  scv->pp = pp;
  irq_permit(q);

  // A capture may already be frozen (e.g. fault-triggered before connect).
  if(scv->sc->state == SIGCAPTURE_STATE_READOUT) {
    scv->sc->send_index = 0;
    pushpull_wakeup(pp, PUSHPULL_EVENT_PULL);
  }
  return 0;
}

SERVICE_DEF_PUSHPULL("sigcapture", 0, 0, sc_vllp_open);


sigcapture_t *
sigcapture_create_vllp(size_t depth_power_of_2, size_t channels,
                       const sigcapture_desc_t channel_descriptors[],
                       uint32_t nominal_frequency)
{
  if(g_scv.sc != NULL)
    return NULL;  // Single instance only.

  sigcapture_t *sc = sigcapture_alloc(depth_power_of_2, channels,
                                      channel_descriptors, nominal_frequency);
  if(sc == NULL)
    return NULL;

  sc->on_complete = sc_vllp_on_complete;
  sc->on_complete_opaque = &g_scv;

  g_scv.sc = sc;
  g_scv.pp = NULL;
  return sc;
}
