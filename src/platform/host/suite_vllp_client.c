/*
 * vllp-client: the mios VLLP *client* (src/net/vllp.c, vllp_client_create)
 * driven against the mios VLLP *server* (the same file, the other role),
 * both in one host-mios binary, over a looped-back virtual CAN bus, in
 * virtual time.
 *
 * The client is the new code; the server has been in service for a while
 * and is the reference here. That asymmetry is what makes this test
 * useful: every per-session convention the two ends have to agree on --
 * the cookie that seeds the CRC, the mirrored per-channel CRC IVs, the
 * channel-management opcodes, who allocates channel ids -- is checked
 * against an implementation that was not written from the same notes.
 * Only one side can be wrong.
 *
 * Topology. All endpoints live in this process and share one vcan. A
 * simulation thread (loopback_fn) moves each frame mios transmits back
 * into mios's receive ring unchanged, so a frame sent on signal id N is
 * delivered to whichever endpoint has rxid N -- exactly one, since the
 * two ends of a link use opposite ids. mios never delivers locally
 * emitted dsig to itself, so nothing short-circuits.
 *
 * The loopback (and the fault injection that rides on it) is shared with
 * the other suites -- see vcan_loop.h.
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/param.h>

#include <mios/vllp.h>
#include <mios/pushpull.h>
#include <mios/task.h>
#include <mios/service.h>

#include "net/pbuf.h"

#include "hosttest.h"
#include "sim.h"
#include "vcan.h"
#include "vcan_loop.h"

#define SEC 1000000ull

#define MTU 64

/* Frame header bits, from docs/vllp.txt. Private to vllp.c, so restated. */
#define VLLP_HDR_S_TEST 0x80

/* Signal ids. Link i uses (0x200 + 2i) client->server and (0x201 + 2i)
   the other way, so a single loopback delivers each frame to exactly one
   endpoint. */
#define LINK_C2S(i) (0x200 + (i) * 2)
#define LINK_S2C(i) (0x201 + (i) * 2)

static int fails;

#define LCHECK(cond, ...)                                        \
  do { if(!(cond)) { fails++;                                    \
       hosttest_check(0, __FILE__, __LINE__, __VA_ARGS__); } } while(0)


/* ---------------- a pushpull app that records what it gets ------------- */

/* Stands in for whatever an application binds to a client channel. Keeps
   the received bytes in one flat buffer and hands out queued messages on
   pull(), which is enough to check ordering, framing and reconnects. */

#define APP_RX_SIZE 8192
#define APP_TX_SIZE 2048

typedef struct app {
  pushpull_t *pp;

  mutex_t mutex;
  cond_t cond;

  uint8_t rx[APP_RX_SIZE];
  size_t rx_used;
  int rx_msgs;

  uint8_t tx[APP_TX_SIZE];
  size_t tx_used;

  int opens;          /* how many times a channel was bound */
  int closes;         /* ...and unbound */
  int live;           /* a channel is currently bound */
  char last_close[64];
} app_t;


static uint32_t
app_push(void *opaque, pbuf_t *pb)
{
  app_t *a = opaque;

  mutex_lock(&a->mutex);
  for(pbuf_t *p = pb; p != NULL; p = p->pb_next) {
    const size_t n = MIN(p->pb_buflen, APP_RX_SIZE - a->rx_used);
    memcpy(a->rx + a->rx_used, pbuf_cdata(p, 0), n);
    a->rx_used += n;
  }
  a->rx_msgs++;
  cond_broadcast(&a->cond);
  mutex_unlock(&a->mutex);

  pbuf_free(pb);
  return 0;
}


static int
app_may_push(void *opaque)
{
  app_t *a = opaque;
  return a->rx_used < APP_RX_SIZE;
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
      const size_t n = MIN(a->tx_used, MIN(a->pp->max_fragment_size,
                                           PBUF_DATA_SIZE -
                                           a->pp->preferred_offset));
      memcpy(pbuf_append(pb, n), a->tx, n);
      memmove(a->tx, a->tx + n, a->tx_used - n);
      a->tx_used -= n;
      cond_broadcast(&a->cond);
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
  a->closes++;
  a->live = 0;
  snprintf(a->last_close, sizeof(a->last_close), "%s",
           reason ? reason : "(none)");
  /* Anything still queued belonged to the session that just went away. */
  a->tx_used = 0;
  cond_broadcast(&a->cond);
  mutex_unlock(&a->mutex);
}


static const pushpull_app_fn_t app_fn = {
  .push = app_push,
  .may_push = app_may_push,
  .pull = app_pull,
  .close = app_close,
};


/* The bind callback: mios calls this from net context for every new
   session, and expects a fresh app bound to the channel each time. */
static error_t
app_open(void *opaque, pushpull_t *pp)
{
  app_t *a = opaque;

  mutex_lock(&a->mutex);
  a->opens++;
  a->live = 1;
  mutex_unlock(&a->mutex);

  a->pp = pp;
  pp->app = &app_fn;
  pp->app_opaque = a;
  return 0;
}


static app_t *
app_create(void)
{
  app_t *a = calloc(1, sizeof(app_t));
  mutex_init(&a->mutex, "appmtx");
  cond_init(&a->cond, "appcond");
  return a;
}


static void
app_send(app_t *a, const void *data, size_t len)
{
  mutex_lock(&a->mutex);
  const size_t n = MIN(len, APP_TX_SIZE - a->tx_used);
  memcpy(a->tx + a->tx_used, data, n);
  a->tx_used += n;
  mutex_unlock(&a->mutex);

  /* Tell the engine there is something to pull. */
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


static void
app_rx_clear(app_t *a)
{
  mutex_lock(&a->mutex);
  a->rx_used = 0;
  a->rx_msgs = 0;
  mutex_unlock(&a->mutex);
}


/* Wait until the app has received at least `want` bytes. */
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


static int
app_wait_live(app_t *a, int live, uint64_t timeout)
{
  const uint64_t deadline = clock_get() + timeout;
  while(a->live != live) {
    if(clock_get() >= deadline)
      return 0;
    usleep(10000);
  }
  return 1;
}


/* ---------------- phases ---------------- */

typedef struct link {
  vllp_t *client;
  vllp_t *server;
  app_t *app;
  vllp_bind_t *bind;
  vcan_loop_t *lb;
} link_t;


static void
fill_pattern(uint8_t *buf, size_t len, uint32_t seq)
{
  for(size_t i = 0; i < len; i++)
    buf[i] = (uint8_t)(seq * 31 + i * 7);
}


/* The handshake, and that the client really did drive it: a client SYNs,
   a server answers, and the bind opens a channel to a real service. */
static void
phase_connect(link_t *l)
{
  hosttest_log("-- connect");

  LCHECK(app_wait_live(l->app, 1, 10 * SEC),
         "connect: bind never opened (opens=%d closes=%d)",
         l->app->opens, l->app->closes);
  LCHECK(l->app->opens == 1, "connect: %d opens, want 1", l->app->opens);
}


/* Echo, both directions, over a client-opened channel. The remote is the
   real svc_echo.c, so this is the whole path: app -> pull -> fragment ->
   loopback -> server reassembly -> service -> back again. */
static void
phase_echo(link_t *l)
{
  hosttest_log("-- echo");

  static const size_t sizes[] = { 1, 7, 8, 9, 63, 64, 200, 500 };
  uint8_t tx[512];

  for(size_t i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++) {
    const size_t len = sizes[i];
    app_rx_clear(l->app);
    fill_pattern(tx, len, i + 1);
    app_send(l->app, tx, len);

    if(!app_wait_rx(l->app, len, 10 * SEC)) {
      LCHECK(0, "echo: %zu bytes never came back (got %zu)", len,
             app_rx_used(l->app));
      continue;
    }
    LCHECK(app_rx_used(l->app) == len,
           "echo: %zu bytes out, %zu back", len, app_rx_used(l->app));
    LCHECK(!memcmp(l->app->rx, tx, len), "echo: %zu byte payload differs",
           len);
  }
}


/* A bind whose service the peer does not have. The open must be refused
   cleanly, retried on a backoff rather than as fast as the bus allows,
   and -- the part that matters -- must not poison the link for anyone
   else. The server bumps its per-channel CRC IV counter for every OPEN it
   receives, including ones it refuses, so if the client does not do the
   same every later channel on that link fails its CRC. */
static void
phase_open_refused(link_t *l)
{
  hosttest_log("-- refused open");

  app_t *bad = app_create();
  vllp_bind_t *b = vllp_client_bind(l->client, "no-such-service",
                                    app_open, bad);
  LCHECK(b != NULL, "refused: bind failed");

  /* It should be refused, i.e. opened locally then closed again. */
  const uint64_t deadline = clock_get() + 10 * SEC;
  while(bad->closes == 0 && clock_get() < deadline)
    usleep(10000);
  LCHECK(bad->closes > 0, "refused: the open was never refused");
  LCHECK(bad->live == 0, "refused: channel still live after a refusal");
  hosttest_log("   refused with reason '%s'", bad->last_close);

  /* The backoff must be a backoff at both ends of the scale: slow enough
     not to flood a shared bus, but it must actually keep trying. A bind
     that gives up for good is a real failure mode -- a unit that boots
     after the gateway, or one whose service only appears after a firmware
     update, would never get a console. */
  const int retries_at_start = bad->opens;
  uint64_t t0 = clock_get();
  while(clock_get() < t0 + 2 * SEC)
    usleep(100000);
  const int fast = bad->opens - retries_at_start;
  LCHECK(fast <= 2, "refused: retried %d times in 2 s -- the backoff is not "
         "working, this would flood a shared bus", fast);

  t0 = clock_get();
  while(bad->opens <= retries_at_start && clock_get() < t0 + 30 * SEC)
    usleep(100000);
  LCHECK(bad->opens > retries_at_start,
         "refused: the bind never retried -- a service that appears later "
         "would never be picked up");
  hosttest_log("   %d retries in the first 2 s, %d after 30 s", fast,
               bad->opens - retries_at_start);

  /* And the link still works for the channel that was already up. This is
     the CRC-IV check: a desynchronised counter shows up here, not above. */
  app_rx_clear(l->app);
  uint8_t tx[64];
  fill_pattern(tx, sizeof(tx), 0xabc);
  app_send(l->app, tx, sizeof(tx));
  LCHECK(app_wait_rx(l->app, sizeof(tx), 10 * SEC),
         "refused: echo broken after a refused open -- per-channel CRC IVs "
         "have probably drifted out of step with the server");
  LCHECK(!memcmp(l->app->rx, tx, sizeof(tx)),
         "refused: echo payload differs after a refused open");
}


/* A new channel opened after a refusal must also work -- the IV counters
   have to be in step for channels created later, not just for the one
   that predates the refusal. */
static void
phase_open_after_refusal(link_t *l)
{
  hosttest_log("-- open after refusal");

  app_t *a = app_create();
  vllp_bind_t *b = vllp_client_bind(l->client, "echo", app_open, a);
  LCHECK(b != NULL, "after-refusal: bind failed");

  if(!app_wait_live(a, 1, 10 * SEC)) {
    LCHECK(0, "after-refusal: channel never opened");
    return;
  }

  uint8_t tx[100];
  fill_pattern(tx, sizeof(tx), 0xdef);
  app_send(a, tx, sizeof(tx));
  LCHECK(app_wait_rx(a, sizeof(tx), 10 * SEC),
         "after-refusal: echo on a channel opened after a refusal never "
         "came back -- CRC IV counters are out of step");
  LCHECK(!memcmp(a->rx, tx, sizeof(tx)),
         "after-refusal: payload differs");
}


/* Pull the link down long enough for the server to time out, then let it
   back. The client must notice, re-SYN, and re-open its binds onto the
   fresh session; the app must be told the old channel died and be handed
   a new one. This is what happens every time one of the units reboots. */
static void
phase_reconnect(link_t *l, int round)
{
  hosttest_log("-- reconnect (round %d)", round);

  const int opens_before = l->app->opens;
  const int closes_before = l->app->closes;

  /* The link timeout is 3 s at both ends; 6 s of silence takes both down. */
  vcan_loop_outage(l->lb, 6 * SEC);

  LCHECK(l->app->closes > closes_before,
         "reconnect: app was never told the channel died");

  if(!app_wait_live(l->app, 1, 20 * SEC)) {
    LCHECK(0, "reconnect: bind never re-opened (opens=%d closes=%d)",
           l->app->opens, l->app->closes);
    return;
  }
  LCHECK(l->app->opens > opens_before,
         "reconnect: the app was reused instead of re-opened");

  /* And the fresh session actually carries data. */
  app_rx_clear(l->app);
  uint8_t tx[64];
  fill_pattern(tx, sizeof(tx), 0x5150 + round);
  app_send(l->app, tx, sizeof(tx));
  LCHECK(app_wait_rx(l->app, sizeof(tx), 15 * SEC),
         "reconnect: no echo on the new session");
  LCHECK(!memcmp(l->app->rx, tx, sizeof(tx)),
         "reconnect: payload differs on the new session");
}


/* Channel ids are a 14-entry space the client allocates. Exhaust it and
   check the 15th bind is refused rather than reusing an id or scribbling
   past the bitmap. */
static void
phase_exhaust(link_t *l)
{
  hosttest_log("-- channel exhaustion");

  enum { N = 20 };
  app_t *apps[N];
  int live = 0;

  for(int i = 0; i < N; i++) {
    apps[i] = app_create();
    vllp_client_bind(l->client, "echo", app_open, apps[i]);
  }

  /* Give them all a chance to settle. */
  const uint64_t t0 = clock_get();
  while(clock_get() < t0 + 10 * SEC)
    usleep(100000);

  for(int i = 0; i < N; i++)
    live += apps[i]->live;

  /* 14 ids total; one is already taken by the echo bind from
     phase_connect and one by phase_open_after_refusal, and the refused
     bind is idle in backoff. So we cannot say exactly how many of these
     came up -- only that it stopped at the ceiling instead of going past
     it, and that nothing was handed a duplicate id. */
  hosttest_log("   %d of %d extra binds came up", live, N);
  LCHECK(live < N, "exhaust: all %d binds opened -- the id space is only 14 "
         "channels wide, so at least one should have been refused", N);
  LCHECK(live > 0, "exhaust: no bind opened at all");

  /* The link is still healthy. */
  app_rx_clear(l->app);
  uint8_t tx[32];
  fill_pattern(tx, sizeof(tx), 0x99);
  app_send(l->app, tx, sizeof(tx));
  LCHECK(app_wait_rx(l->app, sizeof(tx), 15 * SEC),
         "exhaust: link broken after exhausting the id space");
}


/* A bind that keeps being refused must not consume the link as it
   retries. Channel ids are a 14-entry space and every attempt allocates
   one, so an id that is not returned when the refused channel is torn
   down means that after a dozen or so retries the link has no ids left
   and *every* console on it dies -- while the bind that caused it looks
   perfectly healthy. The mios client releases the id in
   vllp_channel_destroy(); this is what proves it. */
static void
phase_refusal_leak(link_t *l)
{
  hosttest_log("-- refused opens do not consume the link");

  app_t *bad = app_create();
  LCHECK(vllp_client_bind(l->client, "still-no-such-service", app_open,
                          bad) != NULL, "leak: bind failed");

  /* More retry cycles than there are channel ids, so a leak has to show.
     Virtual time, so this is free. */
  const uint64_t t0 = clock_get();
  while(clock_get() < t0 + 80 * SEC)
    usleep(100000);
  hosttest_log("   %d refusals in 80 s", bad->closes);
  LCHECK(bad->closes >= 4, "leak: only %d refusals in 80 s -- the retry "
         "loop stopped, so this phase proved nothing", bad->closes);

  /* The link must still have ids to give out. */
  app_t *fresh = app_create();
  LCHECK(vllp_client_bind(l->client, "echo", app_open, fresh) != NULL,
         "leak: bind failed");
  LCHECK(app_wait_live(fresh, 1, 20 * SEC),
         "leak: a new channel could not be opened after %d refused ones -- "
         "channel ids are not being released when a refused channel is "
         "torn down", bad->closes);

  /* ...and the channel that was already up is untouched. */
  app_rx_clear(l->app);
  uint8_t tx[48];
  fill_pattern(tx, sizeof(tx), 0x77);
  app_send(l->app, tx, sizeof(tx));
  LCHECK(app_wait_rx(l->app, sizeof(tx), 15 * SEC),
         "leak: the established channel stopped working");
}


/* A client must not take any old frame on its rx id for the ACK that
   answers its SYN. On a shared CAN bus there is other traffic, and a
   client that comes "up" on a frame the server never sent goes on to use
   a CRC IV the server does not have -- a link that is established at one
   end only, which then just times out. */
static void
phase_noise(link_t *l)
{
  hosttest_log("-- bus noise is not a SYN response");

  const int opens_before = l->app->opens;

  /* Take both ends down, and hold them down for the injection below. */
  l->lb->blackhole = 1;
  uint64_t t0 = clock_get();
  while(clock_get() < t0 + 6 * SEC)
    usleep(100000);
  LCHECK(l->app->live == 0, "noise: channel still live after an outage");

  /* Now play a data frame at the client: header S=1 on channel 0, which
     is neither a SYN (0x0f) nor ACK-shaped (low five bits 0x1f), so it
     reaches the not-connected path instead of being dropped as a bad
     CRC. */
  l->lb->inject[0] = VLLP_HDR_S_TEST | 0x00;
  l->lb->inject[1] = 'x';
  l->lb->inject[2] = 'y';
  l->lb->inject[3] = 'z';
  l->lb->inject_id = LINK_S2C(0);
  l->lb->inject_len = 4;

  t0 = clock_get();
  while(clock_get() < t0 + 3 * SEC)
    usleep(100000);
  LCHECK(l->lb->inject_len == 0, "noise: the frame was never injected, so "
         "this phase proved nothing");
  LCHECK(l->app->opens == opens_before,
         "noise: the client brought a channel up while the bus was black-"
         "holed -- it accepted a frame that was not the SYN response");

  /* Let the real link back and confirm it recovers properly. */
  l->lb->blackhole = 0;
  LCHECK(app_wait_live(l->app, 1, 25 * SEC),
         "noise: link did not recover after the outage");

  app_rx_clear(l->app);
  uint8_t tx[32];
  fill_pattern(tx, sizeof(tx), 0x33);
  app_send(l->app, tx, sizeof(tx));
  LCHECK(app_wait_rx(l->app, sizeof(tx), 15 * SEC),
         "noise: no echo after recovery");
}


/* ---------------- driver ---------------- */

static int
test_vllp_client(void)
{
  hosttest_log("---- mios VLLP client vs mios VLLP server over vcan ----");

  vcan_t *vcan = vcan_create("vcan0", MTU);

  vcan_loop_t *lb = NULL;

  link_t l = {};

  /* The unit: a server serving whatever services this build has. */
  l.server = vllp_server_create(LINK_S2C(0), LINK_C2S(0), MTU, 3);
  LCHECK(l.server != NULL, "server create failed");

  /* The gateway: a client, plus one bind that should simply stay up. */
  l.client = vllp_client_create(LINK_C2S(0), LINK_S2C(0), MTU, 3);
  LCHECK(l.client != NULL, "client create failed");
  if(l.client == NULL || l.server == NULL)
    return fails + 1;

  l.app = app_create();
  l.bind = vllp_client_bind(l.client, "echo", app_open, l.app);
  LCHECK(l.bind != NULL, "bind failed");

  vcan_set_link(vcan, 1);
  lb = vcan_loop_create(vcan, 0x13579bdf);
  l.lb = lb;

  phase_connect(&l);
  phase_echo(&l);
  phase_open_refused(&l);
  phase_open_after_refusal(&l);
  phase_reconnect(&l, 1);
  phase_reconnect(&l, 2);
  phase_noise(&l);
  phase_echo(&l);
  phase_refusal_leak(&l);
  phase_exhaust(&l);

  hosttest_log("  %u frames crossed the bus", lb->frames);
  lb->stop = 1;
  return fails;
}

HOSTTEST_SUITE("vllp-client", test_vllp_client, 0);
