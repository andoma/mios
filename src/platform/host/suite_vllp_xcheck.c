/*
 * vllp-xcheck: the mios VLLP client against an independent implementation
 * of the server -- the host reference stack (host/dsig/vllp.c, the same
 * code the `dsig` tool ships) compiled into this binary in sim mode.
 *
 * The other client suite runs mios against mios. That catches a great
 * deal, because only one of the two roles is new, but it cannot catch a
 * convention both ends read the same wrong way out of the same source
 * file. The wire has a fair number of those:
 *
 *   - what the MTU byte in the SYN actually holds (the adapted value, not
 *     what the caller passed in)
 *   - the cookie becoming the link CRC IV
 *   - the per-channel IV derivation, and which end inverts it
 *   - the counter that derives those IVs stepping in lockstep
 *   - OPEN / OPEN_RESPONSE encoding on the management channel
 *   - S/E sequencing, the flow bit, and the FDCAN length adaptation
 *
 * Get any of them wrong in a way that is symmetric and mios-vs-mios still
 * passes. This suite is the one that would not.
 *
 * The host stack plays the server, so the roles are the mirror image of
 * suite_vllp.c: there the host client drove the guest server. No loopback
 * here -- the peer is a real second implementation on the other side of
 * the virtual CAN bus.
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/param.h>

#include <mios/vllp.h>
#include <mios/pushpull.h>
#include <mios/task.h>

#include "net/pbuf.h"

#include "hosttest.h"
#include "sim.h"
#include "vcan.h"
#include "../../../host/dsig/vllp_sim_api.h"

#define SEC 1000000ull

#define MTU 64

/* mios is the client: it transmits on GUEST_TX and listens on GUEST_RX. */
#define GUEST_TX 0x400
#define GUEST_RX 0x401

/* The service the host server offers, and how many round trips to make. */
#define XSERVICE "xecho"
#define ROUNDS   24

static int fails;

#define XCHECK(cond, ...)                                        \
  do { if(!(cond)) { fails++;                                    \
       hosttest_check(0, __FILE__, __LINE__, __VA_ARGS__); } } while(0)


/* ---------------- the mios side: a small pushpull app ---------------- */

#define APP_BUF 4096

typedef struct app {
  pushpull_t *pp;
  mutex_t mutex;

  uint8_t rx[APP_BUF];
  size_t rx_used;

  uint8_t tx[APP_BUF];
  size_t tx_used;

  volatile int opens;
  volatile int live;
} app_t;

static app_t g_app;


static uint32_t
app_push(void *opaque, pbuf_t *pb)
{
  app_t *a = opaque;

  mutex_lock(&a->mutex);
  for(pbuf_t *p = pb; p != NULL; p = p->pb_next) {
    const size_t n = MIN(p->pb_buflen, APP_BUF - a->rx_used);
    memcpy(a->rx + a->rx_used, pbuf_cdata(p, 0), n);
    a->rx_used += n;
  }
  mutex_unlock(&a->mutex);

  pbuf_free(pb);
  return 0;
}

static int
app_may_push(void *opaque)
{
  app_t *a = opaque;
  return a->rx_used < APP_BUF;
}

static pbuf_t *
app_pull(void *opaque)
{
  app_t *a = opaque;
  pbuf_t *pb = NULL;

  mutex_lock(&a->mutex);
  if(a->tx_used) {
    pb = pbuf_make(a->pp->preferred_offset, 0);
    if(pb != NULL) {
      const size_t n = MIN(a->tx_used,
                           MIN(a->pp->max_fragment_size,
                               PBUF_DATA_SIZE - a->pp->preferred_offset));
      memcpy(pbuf_append(pb, n), a->tx, n);
      memmove(a->tx, a->tx + n, a->tx_used - n);
      a->tx_used -= n;
    }
  }
  mutex_unlock(&a->mutex);
  return pb;
}

static void
app_close(void *opaque, const char *reason)
{
  app_t *a = opaque;
  mutex_lock(&a->mutex);
  a->live = 0;
  a->tx_used = 0;
  mutex_unlock(&a->mutex);
}

static const pushpull_app_fn_t app_fn = {
  .push = app_push,
  .may_push = app_may_push,
  .pull = app_pull,
  .close = app_close,
};

static error_t
app_open(void *opaque, pushpull_t *pp)
{
  app_t *a = opaque;
  a->pp = pp;
  a->opens++;
  a->live = 1;
  pp->app = &app_fn;
  pp->app_opaque = a;
  return 0;
}

static void
app_send(app_t *a, const void *data, size_t len)
{
  mutex_lock(&a->mutex);
  const size_t n = MIN(len, APP_BUF - a->tx_used);
  memcpy(a->tx + a->tx_used, data, n);
  a->tx_used += n;
  mutex_unlock(&a->mutex);
  if(a->pp != NULL)
    pushpull_wakeup(a->pp, PUSHPULL_EVENT_PULL);
}

static size_t
app_rx_used(app_t *a)
{
  mutex_lock(&a->mutex);
  size_t n = a->rx_used;
  mutex_unlock(&a->mutex);
  return n;
}

static int
app_wait_rx(app_t *a, size_t want, uint64_t timeout)
{
  const uint64_t deadline = clock_get() + timeout;
  while(app_rx_used(a) < want) {
    if(clock_get() >= deadline)
      return 0;
    usleep(10000);
  }
  return 1;
}


/* ---------------- the peer: the host reference stack, as server -------- */

typedef struct peer {
  vcan_t *vcan;
  hvllp_t *v;
  hvllp_channel_t *ch;      /* the channel the guest opened */
  int opens;
  int bad_name;
  int echoed;
  int warnings;
  volatile int ready;
  volatile int done;
} peer_t;

static peer_t g_peer;


static void
peer_tx(void *opaque, const void *data, size_t len)
{
  peer_t *p = opaque;
  vcan_peer_send(p->vcan, GUEST_RX, data, len);
}

static long
peer_recv(void *tr, uint32_t *id, void *buf, size_t buflen, int64_t deadline)
{
  peer_t *p = tr;
  while(1) {
    long n = vcan_peer_recv(p->vcan, id, buf, buflen, (uint64_t)deadline);
    if(n < 0)
      return -1;
    if(*id == GUEST_TX)
      return n;
  }
}

static void
peer_log(void *opaque, int level, const char *msg)
{
  peer_t *p = opaque;
  if(level <= 4 /* LOG_WARNING */) {
    p->warnings++;
    hosttest_log("   peer WARN: %s", msg);
  }
}


/* The guest is asking for a service. Accept exactly one name, so a
   mangled name shows up as a refusal rather than silently working. */
static hvllp_open_channel_result_t
peer_open_channel(void *opaque, const char *name, hvllp_channel_t *vc)
{
  peer_t *p = opaque;
  hvllp_open_channel_result_t r = {};

  p->opens++;

  if(strcmp(name, XSERVICE)) {
    hosttest_log("   peer: refusing unknown service '%s'", name);
    p->bad_name++;
    r.error = -17; /* VLLP_ERR_NOT_FOUND */
    return r;
  }

  /* Sim mode has no rx-dispatch thread, so take the handle and drain it
     from the peer loop instead of asking for callbacks. */
  p->ch = vc;
  return r;
}


static void
peer_fn(void *arg)
{
  peer_t *p = arg;

  p->v = hvllp_create_server(MTU, 3, HVLLP_FDCAN_ADAPTATION, p,
                             peer_tx, peer_log, peer_open_channel);
  hvllp_sim_setup(p->v, 0x5eed1234, p, peer_recv, GUEST_TX);
  hvllp_start(p->v);
  p->ready = 1;

  /* Wait for the guest to establish a link and open its channel. */
  const int64_t dl = clock_get() + 30 * SEC;
  while(p->ch == NULL && clock_get() < dl)
    hvllp_sim_poll(p->v, dl);

  if(p->ch == NULL) {
    hosttest_log("   peer: guest never opened a channel");
    p->done = 1;
    return;
  }

  /* Echo whatever arrives. A blocking read is what pumps the protocol in
     sim mode; a read that times out would mark the channel dead, so give
     it a deadline far longer than the test needs. */
  for(int i = 0; i < ROUNDS; i++) {
    void *data = NULL;
    size_t len = 0;
    if(hvllp_channel_read(p->ch, &data, &len, 60 * SEC) || data == NULL) {
      hosttest_log("   peer: read %d failed", i);
      break;
    }
    hvllp_channel_send(p->ch, data, len);
    hvllp_sim_free(data);
    p->echoed++;
  }

  /* Keep the link alive while the guest checks up on it. */
  hvllp_sim_run(p->v, clock_get() + 10 * SEC);
  p->done = 1;
}


static int
pred_peer_done(void *arg)
{
  peer_t *p = arg;
  return p->done;
}

static int
pred_peer_ready(void *arg)
{
  peer_t *p = arg;
  return p->ready;
}


/* ---------------- the test ---------------- */

static void
fill_pattern(uint8_t *buf, size_t len, uint32_t seq)
{
  for(size_t i = 0; i < len; i++)
    buf[i] = (uint8_t)(seq * 31 + i * 7);
}


static int
test_vllp_xcheck(void)
{
  hosttest_log("---- mios VLLP client vs the host reference server ----");

  vcan_t *vcan = vcan_create("vcan0", MTU);
  g_peer.vcan = vcan;
  mutex_init(&g_app.mutex, "xappmtx");

  sim_thread_create("host-server", peer_fn, &g_peer, 1 << 20);
  CHECK(hosttest_wait(pred_peer_ready, &g_peer, 10 * SEC),
        "peer did not start");

  vllp_t *client = vllp_client_create(GUEST_TX, GUEST_RX, MTU, 3);
  XCHECK(client != NULL, "client create failed");
  if(client == NULL)
    return fails + 1;

  XCHECK(vllp_client_bind(client, XSERVICE, app_open, &g_app) != NULL,
         "bind failed");
  vcan_set_link(vcan, 1);

  /* The handshake and the channel open, against a stack that was written
     from the spec rather than from src/net/vllp.c. */
  const uint64_t dl = clock_get() + 30 * SEC;
  while(!g_app.live && clock_get() < dl)
    usleep(10000);
  XCHECK(g_app.live, "no channel to the reference server (opens=%d, peer "
         "saw %d opens, %d with a name it did not recognise)",
         g_app.opens, g_peer.opens, g_peer.bad_name);
  XCHECK(g_peer.bad_name == 0,
         "the reference server did not recognise the service name -- the "
         "OPEN request is not encoded the way it expects");

  if(!g_app.live) {
    g_peer.done = 1;
    return fails;
  }

  /* Round trips across the interesting size boundaries: under a fragment,
     exactly a fragment, and spanning several. Every one of these exercises
     the message CRC and steps the per-message IV on both ends, so a
     divergence shows up as a mismatch rather than as a hang. */
  static const size_t sizes[] = {
    1, 2, 7, 8, 9, 61, 62, 63, 64, 65, 100, 127, 128, 200, 255, 256,
    300, 400, 500, 501, 502, 503, 504, 505,
  };
  const size_t nsizes = sizeof(sizes) / sizeof(sizes[0]);
  _Static_assert(sizeof(sizes) / sizeof(sizes[0]) == ROUNDS,
                 "the peer echoes exactly ROUNDS messages");

  static uint8_t tx[1024];
  int ok = 0;
  for(size_t i = 0; i < nsizes; i++) {
    const size_t len = sizes[i];

    mutex_lock(&g_app.mutex);
    g_app.rx_used = 0;
    mutex_unlock(&g_app.mutex);

    fill_pattern(tx, len, i + 1);
    app_send(&g_app, tx, len);

    if(!app_wait_rx(&g_app, len, 20 * SEC)) {
      XCHECK(0, "no echo for %zu bytes (got %zu)", len,
             app_rx_used(&g_app));
      break;
    }
    XCHECK(app_rx_used(&g_app) == len,
           "%zu bytes out, %zu back", len, app_rx_used(&g_app));
    XCHECK(!memcmp(g_app.rx, tx, len),
           "%zu byte payload differs -- the two stacks disagree about the "
           "message framing or its CRC", len);
    ok++;
  }

  hosttest_log("   %d/%zu round trips against the reference stack", ok,
               nsizes);
  XCHECK(ok == (int)nsizes, "only %d of %zu round trips completed", ok,
         nsizes);

  CHECK(hosttest_wait(pred_peer_done, &g_peer, 60 * SEC),
        "peer did not finish");
  XCHECK(g_peer.echoed == ROUNDS, "the peer echoed %d of %d messages",
         g_peer.echoed, ROUNDS);
  XCHECK(g_peer.warnings == 0,
         "the reference stack logged %d warnings about our traffic",
         g_peer.warnings);
  XCHECK(g_app.opens == 1,
         "the channel was re-opened %d times, so the link was resetting",
         g_app.opens);

  return fails;
}

HOSTTEST_SUITE("vllp-xcheck", test_vllp_xcheck, 0);
