/*
 * vllp: the real production host VLLP client (host/dsig/vllp.c, compiled
 * with VLLP_SIM) driven against the mios VLLP server (src/net/vllp.c),
 * both in one host-mios binary, over a virtual CAN interface, in virtual
 * time. Exercises BOTH production stacks end to end -- no QEMU, no UDP,
 * no wall-clock waits.
 *
 * The client runs as one simulation thread; its protocol loop is pumped
 * cooperatively (see vllp_sim.h) and every frame crosses the vcan rings.
 * A fault layer in the transport hooks drops / duplicates / corrupts
 * frames in both directions so loss and corruption can be exercised
 * deterministically.
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>

#include <mios/vllp.h>          /* guest server: vllp_server_create() */

#include "hosttest.h"
#include "sim.h"
#include "vcan.h"
#include "../../../host/dsig/vllp_sim_api.h"   /* host client: hvllp_* */

#define SEC 1000000ull

/* The MTU-64 server, from the device's point of view. */
#define DEV_TXID 0x200
#define DEV_RXID 0x201
#define MTU 64

/* Fault injection config (integer percents, 0 = off). */
typedef struct faults {
  int drop;      /* % of frames dropped (each direction)      */
  int dup;       /* % of C>S frames sent twice                */
  int corrupt;   /* % of frames with one flipped bit          */
} faults_t;

typedef struct client_ctx {
  vcan_t *vcan;
  faults_t f;
  uint32_t rng;
  int log_warnings;
} client_ctx_t;

static uint32_t
rng32(client_ctx_t *c)
{
  uint32_t x = c->rng ? c->rng : 0x2545f491;
  x ^= x << 13; x ^= x >> 17; x ^= x << 5;
  c->rng = x;
  return x;
}

static int
roll(client_ctx_t *c, int pct)
{
  return pct > 0 && (rng32(c) % 100) < (uint32_t)pct;
}


/* ---- host client transport hooks (run in the sim thread) ---- */

static void
client_tx(void *opaque, const void *data, size_t len)
{
  client_ctx_t *c = opaque;

  if(roll(c, c->f.drop))
    return;                         /* C>S drop */

  uint8_t frame[80];
  memcpy(frame, data, len);
  if(roll(c, c->f.corrupt) && len > 0)
    frame[rng32(c) % len] ^= 1 << (rng32(c) & 7);   /* C>S corrupt */

  vcan_peer_send(c->vcan, DEV_RXID, frame, len);
  if(roll(c, c->f.dup))
    vcan_peer_send(c->vcan, DEV_RXID, frame, len);  /* C>S duplicate */
}

static long
client_recv(void *tr, uint32_t *id, void *buf, size_t buflen, int64_t deadline)
{
  client_ctx_t *c = tr;
  while(1) {
    long n = vcan_peer_recv(c->vcan, id, buf, buflen, (uint64_t)deadline);
    if(n < 0)
      return -1;
    if(*id != DEV_TXID)
      continue;
    if(roll(c, c->f.drop))
      continue;                     /* S>C drop */
    if(roll(c, c->f.corrupt) && n > 0)
      ((uint8_t *)buf)[rng32(c) % n] ^= 1 << (rng32(c) & 7);   /* S>C corrupt */
    return n;
  }
}

static void
client_log(void *opaque, int level, const char *msg)
{
  client_ctx_t *c = opaque;
  if(level <= 4 /* LOG_WARNING */) {
    c->log_warnings++;
    hosttest_log("  client WARN: %s", msg);
  }
}


/* ---- scenario state and helpers ---- */

typedef struct scenario {
  vcan_t *vcan;
  client_ctx_t ctx;
  hvllp_t *v;
  int failures;
  volatile int done;
} scenario_t;

#define SCHECK(sc, cond, ...) \
  do { if(!(cond)) { (sc)->failures++; \
       hosttest_check(0, __FILE__, __LINE__, __VA_ARGS__); } } while(0)

static void
fill_msg(uint8_t *buf, size_t len, uint32_t seq)
{
  for(size_t i = 0; i < len; i++)
    buf[i] = (uint8_t)(seq * 31 + i * 7);
  if(len >= 4)
    memcpy(buf, &seq, 4);
}

static int
msg_ok(const uint8_t *buf, size_t len, uint32_t seq)
{
  static uint8_t exp[4096];   /* single sim thread */
  if(len > sizeof(exp))
    return 0;
  fill_msg(exp, len, seq);
  return memcmp(buf, exp, len) == 0;
}

static int
wait_connected(scenario_t *sc, int64_t timeout_us)
{
  int64_t dl = clock_get() + timeout_us;
  while(!hvllp_is_connected(sc->v) && clock_get() < dl)
    hvllp_sim_poll(sc->v, dl);
  return hvllp_is_connected(sc->v);
}

static hvllp_channel_t *
open_echo(scenario_t *sc)
{
  return hvllp_channel_create(sc->v, "echo", 0, NULL, NULL, NULL, NULL);
}

#define ECHO_BUF_SIZE 4096

/* Largest message that both the server can reassemble and this file's
   scratch buffers can hold. */
static size_t
echo_max_len(void)
{
  const size_t m = vllp_max_message_size();
  return m > ECHO_BUF_SIZE ? ECHO_BUF_SIZE : m;
}

/* One echo round trip. 0 = ok, 1 = payload mismatch, 2 = link/timeout. */
static int
echo_rt(scenario_t *sc, hvllp_channel_t *ch, size_t len, uint32_t seq)
{
  static uint8_t tx[ECHO_BUF_SIZE];
  fill_msg(tx, len, seq);
  hvllp_channel_send(ch, tx, len);

  void *rx = NULL;
  size_t rlen = 0;
  int err = hvllp_channel_read(ch, &rx, &rlen, 3 * SEC);
  if(err || rx == NULL)
    return 2;
  int r = (rlen == len && msg_ok(rx, rlen, seq)) ? 0 : 1;
  hvllp_sim_free(rx);
  return r;
}


/* ---- phases ---- */

static void
phase_echo_sizes(scenario_t *sc)
{
  hosttest_log("-- echo_sizes");
  hvllp_channel_t *ch = open_echo(sc);
  SCHECK(sc, ch != NULL, "echo_sizes: open failed");
  if(ch == NULL)
    return;

  // Sizes are derived, not fixed. The reassembly limit scales with
  // PBUF_DATA_SIZE (VLLP_MAX_MESSAGE_PBUFS buffers' worth), so a list
  // that fits a roomy pool asks for the impossible on a tight one -- and
  // exceeding it CLOSES the channel (ERR_MTU_EXCEEDED), so the first
  // oversize message makes every later size fail too, which reads as a
  // pile of unrelated breakage rather than one bad assumption.
  //
  // Deriving also tests the interesting sizes at whatever the limit
  // happens to be, instead of only at the one the author had in mind.
  const size_t maxmsg = echo_max_len();
  const size_t fixed[] = { 0, 1, 7, 8, 9, 62, 63, 64, 100, 500, 508, 1000 };
  size_t sizes[sizeof(fixed) / sizeof(fixed[0]) + 3];
  size_t n = 0;
  for(size_t i = 0; i < sizeof(fixed) / sizeof(fixed[0]); i++) {
    if(fixed[i] <= maxmsg)
      sizes[n++] = fixed[i];
  }
  // The boundary itself, which no fixed list would hit by luck.
  if(maxmsg > 1)
    sizes[n++] = maxmsg / 2;
  if(maxmsg > 0)
    sizes[n++] = maxmsg - 1;
  sizes[n++] = maxmsg;

  hosttest_log("   max message %zu bytes, %zu sizes", maxmsg, n);
  for(size_t i = 0; i < n; i++) {
    int r = echo_rt(sc, ch, sizes[i], i + 1);
    SCHECK(sc, r == 0, "echo_sizes: len %zu -> %s", sizes[i],
           r == 1 ? "mismatch" : "no reply");
  }
  hvllp_channel_close(ch, 0, 1);
}

static void
phase_pipelined(scenario_t *sc)
{
  /* Several requests in flight on one channel at once. */
  hosttest_log("-- pipelined");
  enum { WIN = 8, ROUNDS = 40 };
  hvllp_channel_t *ch = open_echo(sc);
  SCHECK(sc, ch != NULL, "pipelined: open failed");
  if(ch == NULL)
    return;
  static uint8_t tx[200];
  uint32_t txseq = 0, rxseq = 0;
  int fail = 0;
  while(rxseq < ROUNDS && !fail) {
    while(txseq < ROUNDS && txseq - rxseq < WIN) {
      size_t len = 1 + (txseq % 180);
      fill_msg(tx, len, txseq);
      hvllp_channel_send(ch, tx, len);
      txseq++;
    }
    void *rx = NULL;
    size_t rlen = 0;
    int err = hvllp_channel_read(ch, &rx, &rlen, 3 * SEC);
    if(err || rx == NULL) {
      SCHECK(sc, 0, "pipelined: read %u failed", rxseq);
      fail = 1;
      break;
    }
    size_t exp = 1 + (rxseq % 180);
    SCHECK(sc, rlen == exp && msg_ok(rx, rlen, rxseq),
           "pipelined: seq %u mismatch", rxseq);
    hvllp_sim_free(rx);
    rxseq++;
  }
  hvllp_channel_close(ch, 0, 1);
  hosttest_log("   %u round trips", rxseq);
}

static void
phase_multichannel(scenario_t *sc)
{
  /* 10 echo channels active at once, driven round-robin. */
  hosttest_log("-- multichannel (10)");
  enum { N = 10, ROUNDS = 20 };
  hvllp_channel_t *ch[N];
  int nopen = 0;
  for(int i = 0; i < N; i++) {
    ch[i] = open_echo(sc);
    if(ch[i] == NULL) {
      SCHECK(sc, 0, "multichannel: only opened %d channels", i);
      break;
    }
    nopen++;
  }
  static uint8_t tx[128];
  for(int r = 0; r < ROUNDS; r++) {
    for(int i = 0; i < nopen; i++) {
      size_t len = 20 + i * 8;
      fill_msg(tx, len, r * N + i);
      hvllp_channel_send(ch[i], tx, len);
    }
    for(int i = 0; i < nopen; i++) {
      size_t len = 20 + i * 8;
      void *rx = NULL;
      size_t rlen = 0;
      int err = hvllp_channel_read(ch[i], &rx, &rlen, 3 * SEC);
      SCHECK(sc, err == 0 && rx != NULL, "multichannel: ch%d read: %s",
             i, hvllp_strerror(err));
      if(rx) {
        SCHECK(sc, rlen == len && msg_ok(rx, rlen, r * N + i),
               "multichannel: ch%d round %d mismatch", i, r);
        hvllp_sim_free(rx);
      }
    }
  }
  for(int i = 0; i < nopen; i++)
    hvllp_channel_close(ch[i], 0, 1);
  hosttest_log("   %d channels x %d rounds", nopen, ROUNDS);
}

static void
phase_chargen(scenario_t *sc)
{
  hosttest_log("-- chargen (download)");
  hvllp_channel_t *ch = hvllp_channel_create(sc->v, "chargen", 0, NULL, NULL,
                                             NULL, NULL);
  SCHECK(sc, ch != NULL, "chargen: open failed");
  if(ch == NULL)
    return;
  int msgs = 0;
  int64_t bytes = 0;
  for(int i = 0; i < 200; i++) {
    void *rx = NULL;
    size_t rlen = 0;
    int err = hvllp_channel_read(ch, &rx, &rlen, 3 * SEC);
    if(err || rx == NULL) {
      SCHECK(sc, 0, "chargen: read %d: %s", i, hvllp_strerror(err));
      break;
    }
    const uint8_t *p = rx;
    int good = 1;
    for(size_t k = 0; k < rlen; k++)
      if(p[k] != (uint8_t)k) { good = 0; break; }
    SCHECK(sc, good, "chargen: bad pattern in msg %d", i);
    bytes += rlen;
    msgs++;
    hvllp_sim_free(rx);
  }
  SCHECK(sc, msgs > 0, "chargen: nothing received");
  hvllp_channel_close(ch, 0, 1);
  hosttest_log("   %d msgs, %u bytes", msgs, (unsigned)bytes);
}

static void
phase_discard(scenario_t *sc)
{
  hosttest_log("-- discard (upload)");
  hvllp_channel_t *ch = hvllp_channel_create(sc->v, "discard", 0, NULL, NULL,
                                             NULL, NULL);
  SCHECK(sc, ch != NULL, "discard: open failed");
  if(ch == NULL)
    return;
  // Same reasoning as phase_echo_sizes(): 508 is over the limit once the
  // pool is tight, and an oversize message closes the channel rather
  // than being dropped, so the upload stalls on the very first message.
  static uint8_t buf[508];
  const size_t len = echo_max_len() < sizeof(buf) ? echo_max_len()
                                                  : sizeof(buf);
  memset(buf, 0xa5, sizeof(buf));
  hosttest_log("   %zu byte messages", len);
  int sent = 0;
  for(int i = 0; i < 200; i++) {
    hvllp_channel_send(ch, buf, len);
    sent++;
    int64_t dl = clock_get() + 3 * SEC;
    while(hvllp_channel_tx_pending(ch) > 0 && clock_get() < dl)
      hvllp_sim_poll(sc->v, dl);
    if(hvllp_channel_tx_pending(ch) != 0) {
      SCHECK(sc, 0, "discard: msg %d did not drain", i);
      break;
    }
  }
  hvllp_channel_close(ch, 0, 1);
  hosttest_log("   %d msgs sent", sent);
}

static void
phase_bidir(scenario_t *sc)
{
  /* Both directions active: pull chargen down while pushing discard up,
     interleaved on the one cooperative client. */
  hosttest_log("-- bidirectional (chargen down + discard up)");
  hvllp_channel_t *cg = hvllp_channel_create(sc->v, "chargen", 0, NULL, NULL,
                                             NULL, NULL);
  hvllp_channel_t *dc = hvllp_channel_create(sc->v, "discard", 0, NULL, NULL,
                                             NULL, NULL);
  SCHECK(sc, cg != NULL && dc != NULL, "bidir: open failed");
  if(cg == NULL || dc == NULL)
    return;
  static uint8_t up[256];
  memset(up, 0x5a, sizeof(up));
  int got = 0, put = 0;
  for(int i = 0; i < 60; i++) {
    /* push one up (drain it) */
    hvllp_channel_send(dc, up, sizeof(up));
    int64_t dl = clock_get() + 3 * SEC;
    while(hvllp_channel_tx_pending(dc) > 0 && clock_get() < dl)
      hvllp_sim_poll(sc->v, dl);
    if(hvllp_channel_tx_pending(dc) == 0)
      put++;
    /* pull one down */
    void *rx = NULL;
    size_t rlen = 0;
    if(hvllp_channel_read(cg, &rx, &rlen, 3 * SEC) == 0 && rx != NULL) {
      const uint8_t *p = rx;
      int good = 1;
      for(size_t k = 0; k < rlen; k++)
        if(p[k] != (uint8_t)k) { good = 0; break; }
      SCHECK(sc, good, "bidir: bad chargen pattern");
      got++;
      hvllp_sim_free(rx);
    }
  }
  SCHECK(sc, got > 0 && put > 0, "bidir: got %d put %d", got, put);
  hvllp_channel_close(cg, 0, 1);
  hvllp_channel_close(dc, 0, 1);
  SCHECK(sc, hvllp_is_connected(sc->v), "bidir: link dropped");
  hosttest_log("   down %d msgs, up %d msgs", got, put);
}

/* Echo under a fault profile. 'expect_resets' distinguishes loss/dup
 * (link must ride through -- every echo eventually succeeds) from corrupt
 * (link resets on CRC failure -- some echoes fail, must recover). Never
 * accept corrupt data. */
static void
phase_faults(scenario_t *sc, const char *name, faults_t f, int expect_resets)
{
  hosttest_log("-- %s (drop=%d dup=%d corrupt=%d)", name, f.drop, f.dup,
               f.corrupt);
  sc->ctx.log_warnings = 0;
  sc->ctx.f = f;

  hvllp_channel_t *ch = open_echo(sc);
  int ok = 0, mism = 0, errs = 0;
  for(int i = 0; i < 60; i++) {
    if(ch == NULL) {
      /* channel died on a reset; wait for the link and reopen */
      sc->ctx.f = (faults_t){0};
      if(!wait_connected(sc, 6 * SEC))
        break;
      sc->ctx.f = f;
      ch = open_echo(sc);
      if(ch == NULL)
        break;
    }
    int r = echo_rt(sc, ch, 1 + (i % 40), i);
    if(r == 0) ok++;
    else if(r == 1) { mism++; }
    else { errs++; hvllp_channel_close(ch, 0, 0); ch = NULL; }
  }
  if(ch != NULL)
    hvllp_channel_close(ch, 0, 1);

  /* stop faults and confirm a real round trip works again. The client's
     "connected" flag can be stale-true after the server dropped, so use an
     actual echo as the liveness test and retry across reconnects. */
  sc->ctx.f = (faults_t){0};
  int recovered = 0;
  int64_t rdl = clock_get() + 20 * SEC;
  while(clock_get() < rdl) {
    wait_connected(sc, 6 * SEC);
    hvllp_channel_t *rc = open_echo(sc);
    if(rc == NULL) { hvllp_sim_run(sc->v, clock_get() + SEC); continue; }
    int r = echo_rt(sc, rc, 64, 0xabc);
    hvllp_channel_close(rc, 0, 0);
    if(r == 0) { recovered = 1; break; }
    hvllp_sim_run(sc->v, clock_get() + SEC); /* let the client time out+reSYN */
  }
  SCHECK(sc, recovered, "%s: link did not recover", name);

  SCHECK(sc, mism == 0, "%s: %d corrupt payloads accepted", name, mism);
  if(expect_resets) {
    SCHECK(sc, ok > 0, "%s: no echo got through", name);
  } else {
    SCHECK(sc, errs == 0 && ok > 0, "%s: %d echoes failed (loss should "
           "retransmit)", name, errs);
  }
  hosttest_log("   ok=%d mism=%d err=%d", ok, mism, errs);
}


static void
scenario_fn(void *arg)
{
  scenario_t *sc = arg;
  sc->ctx.vcan = sc->vcan;
  sc->ctx.rng = 0x12345678;

  hvllp_t *v = hvllp_create_client(MTU, 3, HVLLP_FDCAN_ADAPTATION, &sc->ctx,
                                   client_tx, client_log);
  sc->v = v;
  hvllp_sim_setup(v, 0x1234, &sc->ctx, client_recv, DEV_TXID);
  hvllp_start(v);

  SCHECK(sc, wait_connected(sc, 3 * SEC), "did not connect");

  phase_echo_sizes(sc);
  phase_pipelined(sc);
  phase_multichannel(sc);
  phase_chargen(sc);
  phase_discard(sc);
  phase_bidir(sc);
  phase_faults(sc, "loss-5", (faults_t){ .drop = 5 }, 0);
  phase_faults(sc, "loss-20", (faults_t){ .drop = 20 }, 0);
  phase_faults(sc, "dup-10", (faults_t){ .dup = 10 }, 0);
  phase_faults(sc, "corrupt-1", (faults_t){ .corrupt = 1 }, 1);

  SCHECK(sc, hvllp_is_connected(v), "link not up at end");

  hvllp_destroy(v);
  sc->done = 1;
}

static int
pred_done(void *arg)
{
  scenario_t *sc = arg;
  return sc->done;
}

static int
test_vllp(void)
{
  hosttest_log("---- real host client vs mios server, over vcan ----");

  vcan_t *vcan = vcan_create("vcan0", MTU);
  vllp_server_create(DEV_TXID, DEV_RXID, MTU, 3);
  vcan_set_link(vcan, 1);

  scenario_t *sc = calloc(1, sizeof(scenario_t));
  sc->vcan = vcan;

  sim_thread_create("vllp-client", scenario_fn, sc, 1 << 20);

  CHECK(hosttest_wait(pred_done, sc, 120 * SEC), "scenario did not finish");
  hosttest_log("  finished at t=%u us virtual time", (unsigned)clock_get());
  return sc->failures;
}

// Registered twice, same test function, different pool sizes. Both
// matter and they exercise different code:
//
//   vllp        the platform default (512), a roomy pool. Messages up to
//               2044 bytes, generous tx headroom, few fragments.
//   vllp-tight  72, which is what a CAN-only target ends up with when
//               sized for one max-size CAN-FD payload (64) plus dsig's
//               4-byte signal-id prefix. Every frame nearly fills a
//               buffer, so the tx path has almost no headroom to reserve
//               and messages fragment across the reassembly limit.
//
// The tight case is the one that catches things: a transmit path that
// reserves generous headroom is perfectly happy against 512 and overruns
// 72. Keep both green.
HOSTTEST_SUITE_EX("vllp", test_vllp, 0, 0);
HOSTTEST_SUITE_EX("vllp-tight", test_vllp, 0, 72);
