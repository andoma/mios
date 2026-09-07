#include <mios/vcon_pushpull.h>
#include <mios/vcon.h>
#include <mios/pushpull.h>
#include <mios/stream.h>

#include <sys/param.h>

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <malloc.h>

#include "net/pbuf.h"

// Binds a virtual console to a pushpull channel: console output arrives as
// messages and is appended to the scrollback, keystrokes typed by attached
// clients go back out as messages.
//
// The usual consumer is a VLLP client channel opened against a remote
// unit's "shell" service, so that `attach <unit>` on a gateway gives you a
// console on the unit (see vllp_client_bind). Nothing here knows about
// VLLP though -- it is a pushpull app like the ones in net/service, and
// works over any transport that speaks pushpull.
//
// No thread. push()/pull() run in net context, and keystrokes reach us
// through vcon_set_backend_notify() from whichever thread the attached
// client runs on. That matters when a gateway carries a console for each
// of a dozen units: a pump thread per unit would be a dozen stacks.

typedef struct vcon_pp {
  pushpull_t *vp_pp;
  vcon_t *vp_vc;
  uint8_t vp_dead;   // the channel is gone; stop touching vp_pp
} vcon_pp_t;


// Console output from the far end. vcon_backend()'s write side only
// appends to the scrollback ring (dropping oldest when full) and never
// blocks, so this is safe to do on the net thread.
static uint32_t
vcon_pp_push(void *opaque, pbuf_t *pb)
{
  vcon_pp_t *vp = opaque;
  stream_t *backend = vcon_backend(vp->vp_vc);

  for(pbuf_t *p = pb; p != NULL; p = p->pb_next) {
    if(p->pb_buflen)
      stream_write(backend, pbuf_cdata(p, 0), p->pb_buflen, 0);
  }

  pbuf_free(pb);
  return 0;
}


// Always ready. The scrollback ring drops the oldest bytes rather than
// refusing new ones, so there is no state in which we want the peer to
// stop sending -- and clearing the rx-flow-bit for a channel that never
// rejects anything confuses the peer's flow bookkeeping (see the note in
// vllp_refresh_local_flow_status).
static int
vcon_pp_may_push(void *opaque)
{
  return 1;
}


// Keystrokes toward the far end. Takes as much as the fifo has in one
// message: the per-message CRC and the link's one-fragment-at-a-time
// window make a burst of one-byte messages far more expensive than a
// single larger one, and a fast typist or a pasted line arrives as a
// burst.
static pbuf_t *
vcon_pp_pull(void *opaque)
{
  vcon_pp_t *vp = opaque;
  pushpull_t *pp = vp->vp_pp;

  const size_t offset = pp->preferred_offset;
  size_t room = MIN(pp->max_fragment_size, PBUF_DATA_SIZE - offset);
  if(room == 0)
    return NULL;

  // Peek before allocating. pull() is polled for every established
  // channel on every transmit opportunity, so allocating first would have
  // a gateway with a dozen idle consoles take and release a dozen buffers
  // each time round -- and on a tight pool that can be the buffer the ACK
  // path needed. Peeking also means a failed allocation leaves the
  // keystrokes queued instead of eating them.
  uint8_t buf[64];
  const size_t n = vcon_input_peek(vp->vp_vc, buf, MIN(room, sizeof(buf)));
  if(n == 0)
    return NULL;

  pbuf_t *pb = pbuf_make(offset, 0);
  if(pb == NULL)
    return NULL;   // Nothing consumed; the engine re-polls us later

  memcpy(pbuf_append(pb, n), buf, n);
  vcon_input_consume(vp->vp_vc, n);
  return pb;
}


// The channel is gone -- link down, peer closed, or the open was refused.
// Leave a mark in the scrollback: an operator watching an attached console
// should be able to tell "the unit went away" from "the unit went quiet".
static void
vcon_pp_close(void *opaque, const char *reason)
{
  vcon_pp_t *vp = opaque;
  char msg[96];

  vp->vp_dead = 1;
  vcon_set_backend_notify(vp->vp_vc, NULL, NULL);

  const int len = snprintf(msg, sizeof(msg), "\n[disconnected: %s]\n",
                           reason ? reason : "unknown");
  stream_write(vcon_backend(vp->vp_vc), msg, MIN((size_t)len, sizeof(msg) - 1),
               0);

  free(vp);
}


static const pushpull_app_fn_t vcon_pp_fn = {
  .push = vcon_pp_push,
  .may_push = vcon_pp_may_push,
  .pull = vcon_pp_pull,
  .close = vcon_pp_close,
};


// A keystroke arrived on some other thread; ask the engine to pull.
static void
vcon_pp_notify(void *opaque)
{
  vcon_pp_t *vp = opaque;
  if(!vp->vp_dead)
    pushpull_wakeup(vp->vp_pp, PUSHPULL_EVENT_PULL);
}


error_t
vcon_pushpull_open(vcon_t *vc, pushpull_t *pp)
{
  vcon_pp_t *vp = xalloc(sizeof(vcon_pp_t), 0, MEM_MAY_FAIL | MEM_CLEAR);
  if(vp == NULL)
    return ERR_NO_MEMORY;

  vp->vp_pp = pp;
  vp->vp_vc = vc;

  pp->app = &vcon_pp_fn;
  pp->app_opaque = vp;

  // Anything an attached client typed before now is stale -- it was aimed
  // at a session that no longer exists.
  vcon_input_flush(vc);
  vcon_set_backend_notify(vc, vcon_pp_notify, vp);

  static const char banner[] = "\n[connected]\n";
  stream_write(vcon_backend(vc), banner, sizeof(banner) - 1, 0);
  return 0;
}
