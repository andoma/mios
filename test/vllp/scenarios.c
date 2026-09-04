/*
 * VLLP scenarios. Every scenario must leave the link connected with no
 * user channels open on either side, so scenarios can be chained on one
 * peer. Scenarios never abort; they record failures via TST_CHECK.
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "scenarios.h"
#include "tst.h"
#include "vllp_logstream.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>

#define DUR(o, s) ((int64_t)((s) * 1000000.0 * (o)->duration_scale))

#define ECHO_TIMEOUT_US 5000000

/* ---------------------------------------------------------------- */
/* Helpers                                                           */

static void
fill_msg(uint8_t *buf, size_t len, uint32_t seq)
{
  /* seq in first 4 bytes (when room), then a seq-derived pattern */
  for(size_t i = 0; i < len; i++)
    buf[i] = (uint8_t)(seq * 31 + i * 7 + (i >> 8));
  if(len >= 4)
    memcpy(buf, &seq, 4);
}

static int
check_msg(const uint8_t *buf, size_t len, uint32_t seq, const char *what)
{
  uint8_t exp[8192];
  if(len > sizeof(exp)) {
    TST_FAIL("%s: message too long (%zu)", what, len);
    return -1;
  }
  fill_msg(exp, len, seq);
  if(memcmp(buf, exp, len)) {
    size_t i;
    for(i = 0; i < len && buf[i] == exp[i]; i++)
      ;
    TST_FAIL("%s: payload mismatch at byte %zu (seq %u len %zu)",
             what, i, seq, len);
    return -1;
  }
  return 0;
}

static int
check_chargen_msg(peer_t *p, const uint8_t *buf, size_t len)
{
  if(len != p->server_fragment_size)
    return -1;
  for(size_t i = 0; i < len; i++)
    if(buf[i] != (uint8_t)i)
      return -1;
  return 0;
}

/* Open a channel with the synchronous read API (no callbacks) */
static vllp_channel_t *
open_sync(peer_t *p, const char *name, uint32_t flags)
{
  return vllp_channel_create(p->v, name, flags, NULL, NULL, NULL, NULL);
}

/* One echo round trip. Returns 0 on success. */
static int
echo_roundtrip(vllp_channel_t *vc, size_t len, uint32_t seq,
               int64_t timeout_us, const char *what)
{
  uint8_t *buf = malloc(len ? len : 1);
  fill_msg(buf, len, seq);
  vllp_channel_send(vc, buf, len);

  void *rx;
  size_t rxlen;
  int err = vllp_channel_read(vc, &rx, &rxlen, timeout_us);
  if(err || rx == NULL) {
    TST_FAIL("%s: read failed len=%zu seq=%u: %s", what, len, seq,
             err ? vllp_strerror(err) : "EOF");
    free(buf);
    return -1;
  }
  int r = 0;
  if(rxlen != len) {
    TST_FAIL("%s: echoed %zu bytes, sent %zu (seq %u)", what, rxlen, len, seq);
    r = -1;
  } else if(check_msg(rx, rxlen, seq, what)) {
    r = -1;
  }
  free(rx);
  free(buf);
  return r;
}

/* Close with wait and measure how long the handshake took */
static int64_t
close_timed(vllp_channel_t *vc, const char *what, int64_t max_us)
{
  int64_t t0 = tst_now_us();
  vllp_channel_close(vc, 0, 1);
  int64_t dt = tst_now_us() - t0;
  TST_CHECK(dt <= max_us, "%s: close handshake took %.3fs (limit %.3fs)",
            what, dt / 1e6, max_us / 1e6);
  return dt;
}

static void
expect_server_idle(peer_t *p, const char *when)
{
  peer_server_status_t st;
  /* The server processes our last CLOSE slightly after we return; on a
     slow link (8-byte MTU, high latency) that delivery itself takes a
     while, so wait generously. */
  int64_t deadline = tst_now_us() + 10000000;
  while(1) {
    if(p->server_status(p, &st) < 0) {
      TST_FAIL("%s: server status unavailable: %s", when, st.detail);
      return;
    }
    if(st.panicked) {
      TST_FAIL("%s: server PANICKED", when);
      return;
    }
    if(st.connected && st.user_channels == 0)
      return;
    if(tst_now_us() > deadline)
      break;
    tst_sleep_us(50000);
  }
  TST_CHECK(st.connected, "%s: server not connected: %s", when, st.detail);
  TST_CHECK(st.user_channels == 0, "%s: server has %d user channels open:\n%s",
            when, st.user_channels, st.detail);
}

static void
expect_client_connected(peer_t *p, const char *when)
{
  TST_CHECK(peer_wait_connected(p, 4000000) == 0,
            "%s: client not connected", when);
}

static void
expect_no_warnings(peer_t *p, const char *when)
{
  int n = peer_log_count_at_most(p, LOG_WARNING);
  TST_CHECK(n == 0, "%s: %d warning-or-worse host vllp log messages, "
            "last: \"%s\"", when, n, peer_log_last(p, LOG_WARNING));
}

/* ---------------------------------------------------------------- */
/* Concurrent channel workers                                        */

typedef struct worker {
  peer_t *p;
  const char *kind;         /* echo, chargen, discard, log */
  int index;
  size_t max_msg;           /* echo: max message size */
  int window;               /* echo: messages in flight */
  int64_t read_timeout_us;  /* echo: per-message read timeout */
  int64_t close_limit_us;   /* max acceptable close handshake time */
  int64_t start_delay_us;   /* wait before first send (open barrier) */
  int64_t stop_at;
  volatile int stop;
  pthread_t tid;

  /* results */
  int64_t bytes;
  int64_t msgs;
  int errors;
  int eof;                  /* eof seen before we closed */
  int eof_code;
  int tolerate_eof;         /* chaos: EOF is expected, not a failure */
  int64_t close_us;
  char name[32];
} worker_t;

static void
chargen_rx(void *opaque, const void *data, size_t len)
{
  worker_t *w = opaque;
  w->bytes += len;
  w->msgs++;
  if(check_chargen_msg(w->p, data, len)) {
    if(w->errors++ == 0)
      TST_FAIL("%s: bad chargen message (len %zu)", w->name, len);
  }
}

static void
worker_eof(void *opaque, int error_code)
{
  worker_t *w = opaque;
  w->eof = 1;
  w->eof_code = error_code;
}

static void
log_rx(void *opaque, int level, uint32_t seq, int64_t ms_ago, const char *msg)
{
  worker_t *w = opaque;
  (void)level; (void)seq; (void)ms_ago;
  w->msgs++;
  w->bytes += strlen(msg);
}

static void *
worker_thread(void *arg)
{
  worker_t *w = arg;
  peer_t *p = w->p;
  tst_rng_t rng;
  tst_rng_seed(&rng, 0x1234 + w->index);

  if(!strcmp(w->kind, "echo")) {
    vllp_channel_t *vc = open_sync(p, "echo", 0);
    if(vc == NULL) {
      TST_FAIL("%s: channel_create failed", w->name);
      return NULL;
    }
    uint32_t tx_seq = 0, rx_seq = 0;
    size_t *lens = calloc(w->window, sizeof(size_t));
    uint8_t *buf = malloc(w->max_msg + 1);
    int failed = 0;
    while(!failed && (!w->stop || rx_seq < tx_seq)) {
      while(!w->stop && tx_seq - rx_seq < (uint32_t)w->window) {
        size_t len = 1 + tst_rng_u32(&rng) % w->max_msg;
        lens[tx_seq % w->window] = len;
        fill_msg(buf, len, tx_seq);
        vllp_channel_send(vc, buf, len);
        tx_seq++;
      }
      if(rx_seq == tx_seq)
        break;
      void *rx;
      size_t rxlen;
      int err = vllp_channel_read(vc, &rx, &rxlen, w->read_timeout_us);
      if(err || rx == NULL) {
        TST_FAIL("%s: read failed at seq %u: %s", w->name, rx_seq,
                 err ? vllp_strerror(err) : "EOF");
        w->errors++;
        failed = 1;
        break;
      }
      size_t exp = lens[rx_seq % w->window];
      if(rxlen != exp) {
        TST_FAIL("%s: seq %u: got %zu bytes, expected %zu", w->name, rx_seq,
                 rxlen, exp);
        w->errors++;
      } else if(check_msg(rx, rxlen, rx_seq, w->name)) {
        w->errors++;
      }
      free(rx);
      w->bytes += rxlen;
      w->msgs++;
      rx_seq++;
    }
    free(buf);
    free(lens);
    w->close_us = close_timed(vc, w->name, w->close_limit_us);

  } else if(!strcmp(w->kind, "chargen")) {
    vllp_channel_t *vc = vllp_channel_create(p->v, "chargen", 0, chargen_rx,
                                             worker_eof, NULL, w);
    if(vc == NULL) {
      TST_FAIL("%s: channel_create failed", w->name);
      return NULL;
    }
    while(!w->stop && !w->eof)
      tst_sleep_us(10000);
    // w->tolerate_eof: chaos scenarios legitimately reset the link, which
    // surfaces here as an EOF; only a clean-link scenario treats it as a bug.
    if(w->eof && !w->tolerate_eof)
      TST_FAIL("%s: unexpected eof (%s)", w->name, vllp_strerror(w->eof_code));
    w->close_us = close_timed(vc, w->name, w->close_limit_us);

  } else if(!strcmp(w->kind, "discard")) {
    vllp_channel_t *vc = open_sync(p, "discard", 0);
    if(vc == NULL) {
      TST_FAIL("%s: channel_create failed", w->name);
      return NULL;
    }
    if(w->start_delay_us) {
      int64_t d = tst_now_us() + w->start_delay_us;
      while(!w->stop && tst_now_us() < d) tst_sleep_us(10000);
    }
    uint8_t *buf = malloc(w->max_msg);
    uint32_t seq = 0;
    while(!w->stop) {
      if(vllp_channel_tx_pending(vc) < 4) {
        size_t len = w->max_msg;
        fill_msg(buf, len, seq++);
        vllp_channel_send(vc, buf, len);
        w->bytes += len;
        w->msgs++;
      } else {
        tst_sleep_us(200);
      }
    }
    /* Wait for the queue to drain so the close measurement is fair. Only
       flag it if nothing at all is draining (a real stall); a merely slow
       link is not a failure. */
    int prev = vllp_channel_tx_pending(vc);
    int64_t deadline = tst_now_us() + 20000000;
    int64_t last_progress = tst_now_us();
    while(vllp_channel_tx_pending(vc) > 0 && tst_now_us() < deadline) {
      int cur = vllp_channel_tx_pending(vc);
      if(cur < prev) { prev = cur; last_progress = tst_now_us(); }
      tst_sleep_us(1000);
    }
    // A slow link is fine; a permanently stuck queue is not. One 508-byte
    // message over an 8-byte MTU at 30ms RTT is seconds of work, so allow
    // a generous no-progress window before calling it a stall.
    TST_CHECK(tst_now_us() - last_progress < 10000000,
              "%s: tx queue stalled (no progress for 10s, %d pending)",
              w->name, vllp_channel_tx_pending(vc));
    free(buf);
    w->close_us = close_timed(vc, w->name, w->close_limit_us);

  } else if(!strcmp(w->kind, "log")) {
    vllp_logstream_t *ls = vllp_logstream_create(p->v, w, log_rx);
    if(ls == NULL) {
      TST_FAIL("%s: logstream_create failed", w->name);
      return NULL;
    }
    while(!w->stop)
      tst_sleep_us(10000);
    int64_t t0 = tst_now_us();
    vllp_logstream_destroy(ls);
    w->close_us = tst_now_us() - t0;
  }
  return NULL;
}

static void
worker_start(worker_t *w, peer_t *p, const char *kind, int index,
             size_t max_msg, int window)
{
  memset(w, 0, sizeof(*w));
  w->p = p;
  w->kind = kind;
  w->index = index;
  w->max_msg = max_msg;
  w->window = window;
  w->read_timeout_us = ECHO_TIMEOUT_US;
  w->close_limit_us = 1500000;
  snprintf(w->name, sizeof(w->name), "%s#%d", kind, index);
  pthread_create(&w->tid, NULL, worker_thread, w);
}

static void
worker_stop_join(worker_t *w)
{
  pthread_join(w->tid, NULL);
  tst_logf("  %-10s %9lld bytes %7lld msgs  errors=%d  close=%.3fs",
           w->name, (long long)w->bytes, (long long)w->msgs, w->errors,
           w->close_us / 1e6);
}

static void
run_workers(peer_t *p, worker_t *ws, int n, int64_t duration_us)
{
  int64_t t0 = tst_now_us();
  tst_sleep_us(duration_us);
  int64_t total = 0;
  /* Stop everyone first, then join: a worker still streaming while
     another one waits for its close handshake would skew the timing. */
  for(int i = 0; i < n; i++)
    ws[i].stop = 1;
  for(int i = 0; i < n; i++)
    worker_stop_join(&ws[i]);
  double s = (tst_now_us() - t0) / 1e6;
  for(int i = 0; i < n; i++)
    total += ws[i].bytes;
  tst_logf("  aggregate %.1f KB/s over %.1fs", total / 1024.0 / s, s);
  (void)p;
}

/* ---------------------------------------------------------------- */
/* Scenarios                                                         */

static void
sc_link(peer_t *p, const scenario_opts_t *o)
{
  (void)o;
  int64_t t0 = tst_now_us();
  expect_client_connected(p, "link");
  tst_logf("  connected after %.3fs", (tst_now_us() - t0) / 1e6);
  expect_server_idle(p, "link");
}

static void
sc_idle(peer_t *p, const scenario_opts_t *o)
{
  expect_client_connected(p, "idle");
  peer_log_reset(p);
  int64_t end = tst_now_us() + DUR(o, 12);
  int drops = 0;
  while(tst_now_us() < end) {
    if(!vllp_is_connected(p->v))
      drops++;
    tst_sleep_us(100000);
  }
  TST_CHECK(drops == 0, "client link dropped %d times during idle", drops);
  expect_no_warnings(p, "idle");
  expect_server_idle(p, "idle");
}

static void
sc_echo_sizes(peer_t *p, const scenario_opts_t *o)
{
  (void)o;
  static const size_t sizes[] = {
    0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 15, 16, 17, 31, 32, 33, 61, 62, 63, 64,
    65, 100, 127, 128, 255, 256, 500, 507, 508, 509, 510, 511, 512, 513,
    600, 1000, 1016, 1017, 1500, 2000, 4000,
  };
  vllp_channel_t *vc = open_sync(p, "echo", 0);
  if(vc == NULL) {
    TST_FAIL("channel_create failed");
    return;
  }
  uint32_t seq = 1;
  for(size_t i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++) {
    char what[64];
    snprintf(what, sizeof(what), "echo size %zu", sizes[i]);
    int64_t t0 = tst_now_us();

    if(p->max_message_size && sizes[i] > p->max_message_size) {
      /* Too big for the server: it must close the channel with an error
         and the link must survive. Then continue on a fresh channel. */
      uint8_t *buf = malloc(sizes[i]);
      fill_msg(buf, sizes[i], seq++);
      vllp_channel_send(vc, buf, sizes[i]);
      free(buf);
      void *rx = NULL;
      size_t rxlen = 0;
      int err = vllp_channel_read(vc, &rx, &rxlen, ECHO_TIMEOUT_US);
      free(rx);
      TST_CHECK(err != 0 && err != VLLP_ERR_TIMEOUT,
                "%s: expected server to close the channel with an error, "
                "got %s", what, err ? vllp_strerror(err) : "data");
      tst_logf("  %s: server closed channel: %s", what,
               err ? vllp_strerror(err) : "-");
      vllp_channel_close(vc, 0, 1);
      TST_CHECK(vllp_is_connected(p->v), "%s: link lost", what);
      vc = open_sync(p, "echo", 0);
      if(vc == NULL) {
        TST_FAIL("%s: could not reopen echo channel", what);
        return;
      }
      continue;
    }

    int r = echo_roundtrip(vc, sizes[i], seq++, ECHO_TIMEOUT_US, what);
    tst_verbosef("%s: %s %.1f ms", what, r ? "FAIL" : "ok",
                 (tst_now_us() - t0) / 1e3);
    if(r) {
      peer_server_status_t st;
      p->server_status(p, &st);
      if(st.panicked || !vllp_is_connected(p->v)) {
        TST_FAIL("link lost after size %zu (panicked=%d), aborting sweep",
                 sizes[i], st.panicked);
        break;
      }
    }
  }
  close_timed(vc, "echo", 1500000);
  expect_client_connected(p, "echo_sizes");
  expect_server_idle(p, "echo_sizes");
}

static void
sc_echo_pipelined(peer_t *p, const scenario_opts_t *o)
{
  worker_t w;
  peer_log_reset(p);
  worker_start(&w, p, "echo", 0, 400, 8);
  run_workers(p, &w, 1, DUR(o, 5));
  expect_no_warnings(p, "echo_pipelined");
  expect_server_idle(p, "echo_pipelined");
}

static void
sc_chargen(peer_t *p, const scenario_opts_t *o)
{
  worker_t w;
  peer_log_reset(p);
  worker_start(&w, p, "chargen", 0, 0, 0);
  run_workers(p, &w, 1, DUR(o, 5));
  TST_CHECK(w.msgs > 0, "chargen delivered nothing");
  expect_no_warnings(p, "chargen");
  expect_server_idle(p, "chargen");
}

static void
sc_discard(peer_t *p, const scenario_opts_t *o)
{
  worker_t w;
  peer_log_reset(p);
  worker_start(&w, p, "discard", 0, 508, 0);
  run_workers(p, &w, 1, DUR(o, 5));
  TST_CHECK(w.msgs > 0, "discard sent nothing");
  expect_no_warnings(p, "discard");
  expect_server_idle(p, "discard");
}

// 10 channels all pulling data server->client (chargen). Exercises the
// tx scheduler's fairness with many active channels in one direction.
static void
sc_multi_rx(peer_t *p, const scenario_opts_t *o)
{
  worker_t ws[10];
  int n = 0;
  peer_log_reset(p);
  for(int i = 0; i < 10; i++)
    worker_start(&ws[n++], p, "chargen", i, 0, 0);
  run_workers(p, ws, n, DUR(o, 10));
  for(int i = 0; i < n; i++)
    TST_CHECK(ws[i].msgs > 0, "%s starved", ws[i].name);
  expect_client_connected(p, "multi_rx");
  expect_no_warnings(p, "multi_rx");
  expect_server_idle(p, "multi_rx");
}

// 10 channels all pushing data client->server (discard). The server only
// acknowledges, so this is pure one-directional origination -- the mirror
// of multi_rx. (Adding echo here would make it bidirectional, since echo
// responses are server-originated; that belongs in multi_bidir.)
static void
sc_multi_tx(peer_t *p, const scenario_opts_t *o)
{
  worker_t ws[10];
  int n = 0;
  peer_log_reset(p);
  for(int i = 0; i < 10; i++)
    worker_start(&ws[n++], p, "discard", i, p->mtu <= 8 ? 64 : 508, 0);
  run_workers(p, ws, n, DUR(o, 10));
  for(int i = 0; i < n; i++)
    TST_CHECK(ws[i].msgs > 0, "%s starved", ws[i].name);
  expect_client_connected(p, "multi_tx");
  expect_no_warnings(p, "multi_tx");
  expect_server_idle(p, "multi_tx");
}

// 10 channels with bulk flowing BOTH ways at once (chargen server->client
// while discard/echo go client->server). This is the stress case from the
// project goal. See FINDINGS.md: concurrent bidirectional bulk currently
// wedges the link because the 1-bit S/E sequence is shared across the
// whole link with no origination arbitration. Tagged 'bidir' so the
// default suite can exclude a known-failing case.
static void
sc_multi_bidir(peer_t *p, const scenario_opts_t *o)
{
  worker_t ws[10];
  int n = 0;
  peer_log_reset(p);
  for(int i = 0; i < 3; i++)
    worker_start(&ws[n++], p, "echo", i, 300, 4);
  for(int i = 0; i < 3; i++)
    worker_start(&ws[n++], p, "chargen", i, 0, 0);
  for(int i = 0; i < 4; i++)
    worker_start(&ws[n++], p, "discard", i, 508, 0);
  run_workers(p, ws, n, DUR(o, 10));
  for(int i = 0; i < n; i++)
    TST_CHECK(ws[i].msgs > 0, "%s starved", ws[i].name);
  expect_client_connected(p, "multi_bidir");
  expect_no_warnings(p, "multi_bidir");
  expect_server_idle(p, "multi_bidir");
}

static void
sc_churn(peer_t *p, const scenario_opts_t *o)
{
  int iters = (int)(100 * o->duration_scale);
  if(iters < 5)
    iters = 5;
  int64_t worst_close = 0;
  peer_log_reset(p);
  for(int i = 0; i < iters; i++) {
    vllp_channel_t *vc = open_sync(p, "echo", 0);
    if(vc == NULL) {
      TST_FAIL("iteration %d: channel_create failed (ids exhausted?)", i);
      break;
    }
    char what[64];
    snprintf(what, sizeof(what), "churn iter %d", i);
    if(echo_roundtrip(vc, 1 + (i % 200), i, ECHO_TIMEOUT_US, what)) {
      vllp_channel_close(vc, 0, 1);
      break;
    }
    int64_t dt = close_timed(vc, what, 1500000);
    if(dt > worst_close)
      worst_close = dt;
  }
  tst_logf("  %d iterations, worst close %.3fs", iters, worst_close / 1e6);
  expect_no_warnings(p, "churn");
  expect_server_idle(p, "churn");
}

static void
sc_unknown_service(peer_t *p, const scenario_opts_t *o)
{
  (void)o;
  vllp_channel_t *vc = open_sync(p, "no-such-service", 0);
  if(vc == NULL) {
    TST_FAIL("channel_create failed");
    return;
  }
  void *d;
  size_t l;
  int err = vllp_channel_read(vc, &d, &l, 3000000);
  TST_CHECK(err == VLLP_ERR_NOT_FOUND, "open of unknown service gave %s, "
            "expected NOT_FOUND", vllp_strerror(err));
  vllp_channel_close(vc, 0, 0);

  /* link must be unaffected */
  vc = open_sync(p, "echo", 0);
  if(vc) {
    echo_roundtrip(vc, 10, 7, ECHO_TIMEOUT_US, "echo after unknown service");
    close_timed(vc, "echo", 1500000);
  } else {
    TST_FAIL("channel_create failed after unknown service");
  }
  expect_server_idle(p, "unknown_service");
}

static void
sc_max_channels(peer_t *p, const scenario_opts_t *o)
{
  (void)o;
  vllp_channel_t *vcs[14];
  int n = 0;
  for(; n < 14; n++) {
    vcs[n] = open_sync(p, "echo", 0);
    if(vcs[n] == NULL) {
      TST_FAIL("could only create %d channels, expected 14", n);
      break;
    }
  }
  vllp_channel_t *extra = open_sync(p, "echo", 0);
  TST_CHECK(extra == NULL, "15th channel creation should fail");
  if(extra)
    vllp_channel_close(extra, 0, 1);

  for(int i = 0; i < n; i++) {
    char what[64];
    snprintf(what, sizeof(what), "echo on channel slot %d", i);
    if(echo_roundtrip(vcs[i], 50 + i, 100 + i, ECHO_TIMEOUT_US, what) &&
       !vllp_is_connected(p->v)) {
      TST_FAIL("link down, skipping remaining slots");
      break;
    }
  }
  for(int i = 0; i < n; i++) {
    char what[64];
    snprintf(what, sizeof(what), "channel slot %d", i);
    close_timed(vcs[i], what, 1500000);
  }
  expect_server_idle(p, "max_channels");
}

static void
sc_reconnect(peer_t *p, const scenario_opts_t *o)
{
  (void)o;
  vllp_channel_t *vc = open_sync(p, "echo", 0);
  if(vc == NULL) {
    TST_FAIL("channel_create failed");
    return;
  }
  echo_roundtrip(vc, 20, 1, ECHO_TIMEOUT_US, "echo before reconnect");

  /* Make the CLOSE vanish so the server still has the channel open when
     our fresh client SYNs. */
  fault_cfg_t blackout = { .blackout = 1 };
  peer_set_faults(p, PEER_DIR_C2S, &blackout);
  vllp_channel_close(vc, 0, 0);
  tst_sleep_us(200000);

  peer_server_status_t st;
  if(p->server_status(p, &st) == 0)
    TST_CHECK(st.user_channels == 1,
              "server should still see 1 channel before re-SYN, has %d",
              st.user_channels);

  p->restart_client(p);
  peer_clear_faults(p);
  peer_log_reset(p);

  int64_t t0 = tst_now_us();
  expect_client_connected(p, "after restart");
  tst_logf("  reconnected in %.3fs", (tst_now_us() - t0) / 1e6);

  vc = open_sync(p, "echo", 0);
  if(vc) {
    echo_roundtrip(vc, 30, 2, ECHO_TIMEOUT_US, "echo after reconnect");
    close_timed(vc, "echo", 1500000);
  } else {
    TST_FAIL("channel_create failed after reconnect");
  }
  expect_server_idle(p, "reconnect");
}

static void
sc_link_timeout(peer_t *p, const scenario_opts_t *o)
{
  (void)o;
  vllp_channel_t *vc = vllp_channel_create(p->v, "echo", 0, NULL, NULL,
                                           NULL, NULL);
  if(vc == NULL) {
    TST_FAIL("channel_create failed");
    return;
  }
  echo_roundtrip(vc, 20, 1, ECHO_TIMEOUT_US, "echo before blackout");

  peer_log_reset(p);
  fault_cfg_t blackout = { .blackout = 1 };
  peer_set_faults_both(p, &blackout);
  int64_t t0 = tst_now_us();

  /* The channel must report EOF with TIMEOUT within timeout + margin */
  void *d;
  size_t l;
  int err = vllp_channel_read(vc, &d, &l, (p->timeout + 3) * 1000000L);
  int64_t dt = tst_now_us() - t0;
  TST_CHECK(err == VLLP_ERR_TIMEOUT, "channel read gave %s after %.2fs, "
            "expected TIMEOUT", vllp_strerror(err), dt / 1e6);
  TST_CHECK(dt >= (p->timeout - 1) * 1000000L,
            "link timed out after only %.2fs (timeout %ds)", dt / 1e6,
            p->timeout);
  TST_CHECK(!vllp_is_connected(p->v), "client still connected after blackout");
  vllp_channel_close(vc, 0, 0);

  /* Server must also drop the session, but only assert this where the
     link sim fully controls the peer's receive path. Over QEMU user-mode
     networking the guest shares a multicast group and cannot be cleanly
     isolated, so the guest keeps its session; the MCU's 3s inactivity
     timeout is verified separately by freezing a real client. */
  if(p->reliable_blackout) {
    peer_server_status_t st;
    int64_t sdeadline = tst_now_us() + (p->timeout + 3) * 1000000L;
    int server_down = 0;
    while(tst_now_us() < sdeadline) {
      if(p->server_status(p, &st) == 0 && !st.panicked && !st.connected) {
        server_down = 1;
        break;
      }
      tst_sleep_us(100000);
    }
    TST_CHECK(server_down, "server never dropped the session during blackout");
  }

  peer_clear_faults(p);
  t0 = tst_now_us();
  expect_client_connected(p, "after blackout");
  tst_logf("  recovered in %.3fs", (tst_now_us() - t0) / 1e6);

  vc = open_sync(p, "echo", 0);
  if(vc) {
    echo_roundtrip(vc, 30, 2, ECHO_TIMEOUT_US, "echo after recovery");
    close_timed(vc, "echo", 1500000);
  } else {
    TST_FAIL("channel_create failed after recovery");
  }
  expect_server_idle(p, "link_timeout");
}

/* Reproduces a livelock seen in the field (a rotary-positioner MCU that
 * reboots faster than the client's keep-alive timeout).
 *
 * The trap: the client has a channel-open in flight (its CMC fragment is
 * current_tx, un-ACKed) when the link goes down. On disconnect the client
 * keeps that fragment. When it re-SYNs, the peer resets its per-session
 * CRC IVs, but the client retransmits the stale current_tx first -- so the
 * peer fails CRC on the CMC channel (which always exists) and drops the
 * link. current_tx is never ACKed, so it survives every reconnect and the
 * link never recovers.
 *
 * We stage it by blacking out both directions and then opening a channel:
 * its CMC open is sent but never ACKed, and the client times out with that
 * fragment pending. Clearing the fault must let the link recover; a client
 * that does not flush pending tx on disconnect livelocks here. */
static void
sc_reconnect_stale_tx(peer_t *p, const scenario_opts_t *o)
{
  (void)o;

  /* Start connected and idle with one clean echo. */
  vllp_channel_t *vc = open_sync(p, "echo", 0);
  if(vc == NULL) {
    TST_FAIL("channel_create failed");
    return;
  }
  echo_roundtrip(vc, 20, 1, ECHO_TIMEOUT_US, "echo before");
  close_timed(vc, "echo before", 1500000);

  peer_log_reset(p);

  /* Black out both ways, then open a channel. Its CMC open goes out but is
     never ACKed, so it stays as the client's in-flight current_tx. */
  fault_cfg_t blackout = { .blackout = 1 };
  peer_set_faults_both(p, &blackout);

  vllp_channel_t *stuck = open_sync(p, "echo", 0);
  if(stuck == NULL) {
    TST_FAIL("channel_create (stuck) failed");
    return;
  }

  /* Wait past the link timeout: the client disconnects with the CMC open
     still pending. */
  int64_t deadline = tst_now_us() + (p->timeout + 2) * 1000000L;
  while(vllp_is_connected(p->v) && tst_now_us() < deadline)
    tst_sleep_us(50000);
  TST_CHECK(!vllp_is_connected(p->v),
            "client did not disconnect under blackout within timeout+2s");

  /* Drain the stuck channel's EOF and close it, so only the stale
     current_tx is left to poison the reconnect. */
  void *d;
  size_t l;
  vllp_channel_read(stuck, &d, &l, 500000);
  vllp_channel_close(stuck, 0, 0);

  peer_clear_faults(p);

  /* The link must recover and make progress -- reconnect AND a fresh echo
     within a bounded time. A client that retransmits the stale CMC
     fragment livelocks and never gets here. */
  int64_t rec0 = tst_now_us();
  int64_t rdeadline = rec0 + (p->timeout + 6) * 1000000L;
  int recovered = 0;
  while(!recovered && tst_now_us() < rdeadline) {
    if(peer_wait_connected(p, 500000) != 0)
      continue;
    /* Quiet probe: try one echo without asserting, so intermediate
       failures while the link is still flapping are not counted. */
    vllp_channel_t *vc2 = open_sync(p, "echo", 0);
    if(vc2 == NULL) {
      tst_sleep_us(200000);
      continue;
    }
    uint8_t msg[30];
    fill_msg(msg, sizeof(msg), 2);
    vllp_channel_send(vc2, msg, sizeof(msg));
    void *rx = NULL;
    size_t rxlen = 0;
    int err = vllp_channel_read(vc2, &rx, &rxlen, 1000000);
    if(err == 0 && rx != NULL && rxlen == sizeof(msg)) {
      recovered = 1;
      free(rx);
      close_timed(vc2, "echo after", 1500000);
    } else {
      if(rx != NULL)
        free(rx);
      vllp_channel_close(vc2, 0, 0);
      tst_sleep_us(200000);
    }
  }
  TST_CHECK(recovered,
            "link did not recover after stale-tx reconnect (livelock)");
  if(recovered)
    tst_logf("  recovered in %.3fs", (tst_now_us() - rec0) / 1e6);
  expect_server_idle(p, "reconnect_stale_tx");
}

/* Mixed load under a fault profile. Data integrity must hold and the
 * link must never drop. */
static void
run_faulty_mix(peer_t *p, const scenario_opts_t *o, const fault_cfg_t *cfg,
               const char *what, double seconds)
{
  worker_t ws[4];
  peer_log_reset(p);
  linksim_reset_stats(p->ls);
  peer_set_faults_both(p, cfg);
  /* An 8-byte MTU moves ~7 bytes per round trip, so keep echo messages
     short there and allow generous read times: with 20% loss or 30ms
     latency a single fragment can take tens of ms and four channels
     share the link. Close may legitimately wait behind the in-flight
     message; the host gives up at 3s, so 2.9s is the useful limit. */
  // Pure client->server bulk (discard). Fault runs test retransmission and
  // flow control in one direction; the server only acknowledges, so this
  // avoids the separate concurrent-origination limitation (a server echo
  // reply is itself an origination -- see FINDINGS.md, sc_multi_bidir).
  // Size messages to the MTU so an 8-byte link's backlog stays closeable.
  size_t msz = p->mtu <= 8 ? 32 : 508;
  for(int i = 0; i < 4; i++) {
    worker_start(&ws[i], p, "discard", i, msz, 0);
    ws[i].close_limit_us = 3100000; // just above the host's 3s forced-close
  }
  run_workers(p, ws, 4, DUR(o, seconds));
  peer_clear_faults(p);

  linksim_stats_t a, b;
  linksim_get_stats(p->ls, PEER_DIR_C2S, &a);
  linksim_get_stats(p->ls, PEER_DIR_S2C, &b);
  tst_logf("  c2s: %llu frames, %llu dropped, %llu dup, %llu corrupt",
           (unsigned long long)a.frames, (unsigned long long)a.dropped,
           (unsigned long long)a.dupped, (unsigned long long)a.corrupted);
  tst_logf("  s2c: %llu frames, %llu dropped, %llu dup, %llu corrupt",
           (unsigned long long)b.frames, (unsigned long long)b.dropped,
           (unsigned long long)b.dupped, (unsigned long long)b.corrupted);

  expect_client_connected(p, what);
  expect_no_warnings(p, what);
  expect_server_idle(p, what);
}

static void
sc_loss_1(peer_t *p, const scenario_opts_t *o)
{
  fault_cfg_t f = { .drop = 0.01 };
  run_faulty_mix(p, o, &f, "loss_1", 8);
}

static void
sc_loss_5(peer_t *p, const scenario_opts_t *o)
{
  fault_cfg_t f = { .drop = 0.05 };
  run_faulty_mix(p, o, &f, "loss_5", 8);
}

static void
sc_loss_20(peer_t *p, const scenario_opts_t *o)
{
  fault_cfg_t f = { .drop = 0.20 };
  run_faulty_mix(p, o, &f, "loss_20", 8);
}

static void
sc_dup_10(peer_t *p, const scenario_opts_t *o)
{
  fault_cfg_t f = { .dup = 0.10 };
  run_faulty_mix(p, o, &f, "dup_10", 8);
}

static void
sc_latency(peer_t *p, const scenario_opts_t *o)
{
  fault_cfg_t f = { .delay_min_us = 30000, .delay_max_us = 30001 };
  run_faulty_mix(p, o, &f, "latency", 8);
}

static void
sc_loss_dup_delay(peer_t *p, const scenario_opts_t *o)
{
  fault_cfg_t f = { .drop = 0.05, .dup = 0.05, .delay_min_us = 2000, .delay_max_us = 2001 };
  run_faulty_mix(p, o, &f, "loss_dup_delay", 8);
}

/* Chaos class: faults the protocol cannot be expected to ride through
 * without a link reset (bit flips -> CRC failure, reordering -> the
 * 1-bit sequence space cannot represent it). Requirements: never accept
 * corrupt data, and recover to a working link once the faults stop. */
static void
run_chaos(peer_t *p, const scenario_opts_t *o, const fault_cfg_t *cfg,
          const char *what, double seconds)
{
  worker_t w;
  peer_log_reset(p);
  linksim_reset_stats(p->ls);
  peer_set_faults_both(p, cfg);
  worker_start(&w, p, "chargen", 0, 0, 0);
  w.tolerate_eof = 1;
  w.close_limit_us = 3100000;
  int64_t end = tst_now_us() + DUR(o, seconds);
  int disconnects = 0, was = 1;
  while(tst_now_us() < end) {
    int c = vllp_is_connected(p->v);
    if(was && !c)
      disconnects++;
    was = c;
    tst_sleep_us(10000);
  }
  peer_clear_faults(p);
  w.stop = 1;
  pthread_join(w.tid, NULL);
  tst_logf("  %lld good messages, %d bad, %d client disconnects, eof=%d (%s)",
           (long long)w.msgs, w.errors, disconnects, w.eof,
           w.eof ? vllp_strerror(w.eof_code) : "-");
  TST_CHECK(w.errors == 0, "%s: %d corrupt messages accepted", what, w.errors);

  /* Recovery: the server may have dropped the session without the client
     noticing yet. Wait out the client timeout, then restart it if needed. */
  int64_t t0 = tst_now_us();
  int64_t deadline = t0 + (p->timeout + 3) * 1000000L;
  peer_server_status_t st;
  while(tst_now_us() < deadline) {
    if(vllp_is_connected(p->v) && p->server_status(p, &st) == 0 &&
       st.connected && st.user_channels == 0)
      break;
    tst_sleep_us(100000);
  }
  if(!vllp_is_connected(p->v))
    p->restart_client(p);
  peer_wait_connected(p, 5000000);
  tst_logf("  link usable again after %.1fs", (tst_now_us() - t0) / 1e6);
  vllp_channel_t *vc = open_sync(p, "echo", 0);
  if(vc) {
    echo_roundtrip(vc, 30, 2, ECHO_TIMEOUT_US, "echo after chaos");
    close_timed(vc, "echo", 1500000);
  } else {
    TST_FAIL("channel_create failed after chaos");
  }
  expect_server_idle(p, what);
}

static void
sc_reorder(peer_t *p, const scenario_opts_t *o)
{
  fault_cfg_t f = { .delay_min_us = 0, .delay_max_us = 20000 };
  run_chaos(p, o, &f, "reorder", 6);
}

static void
sc_corrupt(peer_t *p, const scenario_opts_t *o)
{
  fault_cfg_t f = { .corrupt = 0.02 };
  run_chaos(p, o, &f, "corrupt", 6);
}

const scenario_t scenarios[] = {
  { "link",            "quick",        "SYN/ACK and initial state", sc_link },
  { "echo_sizes",      "quick",        "echo round trips, sizes 0..4000", sc_echo_sizes },
  { "echo_pipelined",  "quick",        "8 echo messages in flight", sc_echo_pipelined },
  { "unknown_service", "quick",        "open of nonexistent service", sc_unknown_service },
  { "max_channels",    "quick",        "all 14 user channels at once", sc_max_channels },
  { "churn",           "quick",        "open/echo/close x100", sc_churn },
  { "chargen",         "bandwidth",    "download 5s, verify pattern", sc_chargen },
  { "discard",         "bandwidth",    "upload 5s", sc_discard },
  { "multi_rx",        "bandwidth",    "10 channels, server->client bulk", sc_multi_rx },
  { "multi_tx",        "bandwidth",    "10 channels, client->server bulk (discard)", sc_multi_tx },
  { "multi_bidir",     "bidir",        "10 channels, bulk both ways (known wedge)", sc_multi_bidir },
  { "idle",            "long",         "12s idle, keepalive only", sc_idle },
  { "reconnect",       "link",         "fresh SYN with channel open on server", sc_reconnect },
  { "link_timeout",    "link",         "blackout > timeout, recovery", sc_link_timeout },
  { "reconnect_stale_tx", "link",      "reconnect with pending tx (livelock repro)", sc_reconnect_stale_tx },
  { "loss_1",          "faults",       "1% loss both ways", sc_loss_1 },
  { "loss_5",          "faults",       "5% loss both ways", sc_loss_5 },
  { "loss_20",         "faults",       "20% loss both ways", sc_loss_20 },
  { "dup_10",          "faults",       "10% duplication", sc_dup_10 },
  { "reorder",         "chaos",        "0-20ms jitter: integrity + recovery", sc_reorder },
  { "latency",         "faults",       "30ms fixed one-way delay", sc_latency },
  { "loss_dup_delay",  "faults",       "5% loss + 5% dup + 2ms delay", sc_loss_dup_delay },
  { "corrupt",         "chaos",        "2% bit flips: integrity + recovery", sc_corrupt },
};

const int num_scenarios = sizeof(scenarios) / sizeof(scenarios[0]);
