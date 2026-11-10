/*
 * tcp-soak: src/net/ipv4/tcp.c against a peer that is a small but
 * complete TCP endpoint, over a link that loses, duplicates, delays and
 * reorders frames at random, in virtual time.
 *
 * Where the "tcp" suite checks exactly what Mios does in a number of
 * scripted situations, this one runs many connections with parameters
 * drawn from the seed and only checks what must always hold:
 *
 *  - every byte arrives, once, in order and unchanged, both ways
 *  - the stream ends where it should, and both sides see it end
 *  - Mios stays within the window and segment size the peer gave it,
 *    never acks what was not sent, never resets the connection
 *  - what it reports in SACK blocks is true (the peer never sends
 *    those bytes again, so a false report stalls the connection)
 *  - when it is over the connection is gone on Mios's side
 *
 *   build.host/mios.elf tcp-soak [--seed=N] [--rounds=N] [--conn=N]
 *                                [--verbose] [--trace]
 *
 * A failure prints the parameters of the connection. --conn=N runs
 * that connection alone, to look at one that failed, and --trace
 * prints every segment the peer sends (>) and gets (<).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/param.h>

#include <mios/service.h>
#include <mios/stream.h>
#include <mios/task.h>

#include <net/ether.h>
#include <net/ipv4/tcp.h>

#include "cpu.h"
#include "hosttest.h"
#include "sim.h"
#include "sim_tcp.h"

#define MSEC 1000ull
#define SEC  1000000ull

#define PORT_SINK   7100
#define PORT_SOURCE 7101
#define PORT_ECHO   7102

#define MODE_SINK   0   // The peer sends, Mios reads
#define MODE_SOURCE 1   // Mios sends, the peer reads
#define MODE_ECHO   2   // The peer sends, Mios sends it back

#define STREAM_MAX 262144

static uint8_t
pattern(uint32_t off)
{
  return off ^ ((off >> 8) * 31) ^ ((off >> 16) * 7);
}

static uint32_t
xorshift(uint32_t *state)
{
  uint32_t x = *state;
  x ^= x << 13;
  x ^= x >> 17;
  x ^= x << 5;
  return *state = x;
}


// ---- Mios side: the applications ----

// Set by the script before it connects. One connection at a time
static volatile uint32_t app_seed;
static volatile uint32_t app_source_bytes;
static volatile int app_slow;
static volatile int app_opened;

// For the script to look at afterwards
static volatile uint32_t app_rx;
static volatile int app_bad;
static volatile int app_err;
static volatile int app_done;

static uint32_t app_rng;
static uint8_t app_buf[4096];

static uint32_t
app_rnd(uint32_t n)
{
  return xorshift(&app_rng) % n;
}

static void
app_pause(void)
{
  if(app_rnd(app_slow ? 4 : 32) == 0)
    usleep(app_rnd(app_slow ? 50000 : 5000));
}

// Returns the number of bytes read and found in order, 0 at the end
static ssize_t
app_read(stream_t *s, size_t max)
{
  const ssize_t n = stream_read(s, app_buf, 1 + app_rnd(max), 1);
  if(n <= 0)
    return 0;
  for(ssize_t i = 0; i < n; i++) {
    if(app_buf[i] != pattern(app_rx + i))
      app_bad++;
  }
  app_rx += n;
  return n;
}

static void *
sink_thread(void *arg)
{
  stream_t *s = arg;
  while(app_read(s, 3000))
    app_pause();
  stream_close(s);
  app_done = 1;
  return NULL;
}

static void *
source_thread(void *arg)
{
  stream_t *s = arg;
  uint32_t off = 0;
  uint32_t n = app_source_bytes;
  while(n) {
    const size_t chunk = 1 + app_rnd(MIN(n, sizeof(app_buf)));
    for(size_t i = 0; i < chunk; i++)
      app_buf[i] = pattern(off + i);
    if(stream_write(s, app_buf, chunk, 0) != chunk) {
      app_err++;
      break;
    }
    off += chunk;
    n -= chunk;
    if(app_rnd(8) == 0)
      stream_write(s, NULL, 0, 0); // Flush
    app_pause();
  }
  stream_close(s);
  app_done = 1;
  return NULL;
}

static void *
echo_thread(void *arg)
{
  stream_t *s = arg;
  ssize_t n;
  while((n = app_read(s, 1500)) != 0) {
    if(stream_write(s, app_buf, n, 0) != n) {
      app_err++;
      break;
    }
    if(app_rnd(4) == 0)
      stream_write(s, NULL, 0, 0);
    app_pause();
  }
  stream_close(s);
  app_done = 1;
  return NULL;
}

// Any connection after the first one the script asked for (a late
// copy of its SYN can start one) is just closed
static void *
stale_thread(void *arg)
{
  stream_close(arg);
  return NULL;
}

static error_t
app_open(stream_t *s, void *(*fn)(void *), const char *name)
{
  if(app_opened++)
    fn = stale_thread;
  app_rng = app_seed | 1;
  return thread_create(fn, s, 16384, name, TASK_DETACHED, 5) ?
    0 : ERR_NO_MEMORY;
}

static error_t
sink_open(stream_t *s)
{
  return app_open(s, sink_thread, "soaksink");
}

static error_t
source_open(stream_t *s)
{
  return app_open(s, source_thread, "soaksource");
}

static error_t
echo_open(stream_t *s)
{
  return app_open(s, echo_thread, "soakecho");
}

SERVICE_DEF_STREAM("soak-sink", PORT_SINK, sink_open);
SERVICE_DEF_STREAM_EX("soak-source", PORT_SOURCE, 14, 11, source_open);
SERVICE_DEF_STREAM_EX("soak-echo", PORT_ECHO, 12, 11, echo_open);


// ---- Peer side ----

#define PEER_RTO   (300 * MSEC)
#define PEER_BURST 8             // Segments in flight, a crude cwnd

#define Q_MAX 256

typedef struct {
  uint64_t at;
  uint32_t order;
  uint16_t len;
  uint8_t used;
  uint8_t out;                   // Towards Mios
  uint8_t data[1600];
} qframe_t;

typedef struct soak {
  sim_tcp_t t;                   // Must be first

  uint32_t rng;
  int conn;
  int failed;

  // Parameters
  int mode;
  uint32_t tx_total;             // Bytes the peer sends
  uint32_t rx_expect;            // Bytes Mios sends
  int drop, dup, reorder;        // Per mille
  uint32_t delay;                // One way, us
  uint32_t wnd;                  // The peer's receive buffer
  int seg_random;                // Segments of any size
  int ack_every;                 // 2: delayed ACKs
  int early_fin;                 // Source: close before the data came
  int stall;                     // The peer stops reading now and then

  // Negotiated
  int established;
  int mss;                       // For segments to Mios
  int rx_mss;                    // Largest segment Mios may send
  int ws;                        // Shift of the peer's window field
  int mios_ws;                   // Shift of Mios's window field
  int sack;
  uint32_t mios_isn;

  // Sending. Sequence numbers are stream offsets, the FIN is tx_total
  uint32_t snd_una, snd_nxt, snd_max;
  uint32_t mios_wnd;
  int dupacks;
  int probe;
  uint64_t rto_at;
  uint64_t syn_at;

  // Receiving
  uint32_t rcv_nxt;
  uint32_t rx_high;              // End of the highest byte received
  uint32_t rx_last;              // Start of the last segment kept
  uint32_t edge;                 // Right edge of the window
  uint32_t edge_adv;             // The highest one Mios has been told
  int rx_fin;
  int rx_done;                   // Everything and the FIN received
  int closed;                    // Not reading
  int unacked;
  uint64_t delack_at;
  uint64_t stall_at;

  // What Mios has heard from the peer, see soak_patience()
  uint64_t heard_at;
  int heard_ack;
  int starved;
  int reset;                     // Mios reset the connection

  // Counters, for the log
  int n_in, n_out, n_lost;
  uint64_t last_in;

  uint32_t order;
} soak_t;

static soak_t soak;
static qframe_t soak_q[Q_MAX];
static uint8_t soak_txmap[STREAM_MAX + 1]; // Bytes Mios has SACKed
static uint8_t soak_rxmap[STREAM_MAX + 1]; // Bytes received
static volatile int script_done;
static volatile int soak_aborted;
static int soak_trace;
static vnet_t *soak_vnet;
static uint32_t peer_ip, mios_ip;

static uint32_t
rnd(soak_t *s, uint32_t n)
{
  return xorshift(&s->rng) % n;
}

static void
soak_describe(const soak_t *s)
{
  static const char *modes[] = { "sink", "source", "echo" };
  hosttest_log("conn %d: %s tx=%u rx=%u drop=%d dup=%d reorder=%d "
               "delay=%uus wnd=%u mss=%d wscale=%d sack=%d(%d) segrnd=%d "
               "ackevery=%d earlyfin=%d stall=%d slow=%d",
               s->conn, modes[s->mode], s->tx_total, s->rx_expect,
               s->drop, s->dup, s->reorder, s->delay, s->wnd, s->t.mss,
               s->t.wscale, s->t.sack_permitted, s->sack, s->seg_random,
               s->ack_every, s->early_fin, s->stall, app_slow);
}

// A check on what Mios did. The first failure describes the connection
#define SOAK_CHECK(s, cond, fmt...) do {                                \
    if(!(cond)) {                                                       \
      if(!(s)->failed)                                                  \
        soak_describe(s);                                               \
      (s)->failed = 1;                                                  \
      hosttest_check(0, __FILE__, __LINE__, fmt);                       \
    }                                                                   \
  } while(0)


// -- The link --

static void
link_put(soak_t *s, int out, const uint8_t *f, size_t len)
{
  if(len > sizeof(soak_q[0].data))
    return;

  if(rnd(s, 1000) < s->drop) {
    s->n_lost++;
    if(soak_trace > 1)
      hosttest_log("  lost %s flags=%02x", out ? ">" : "<", f[47]);
    return;
  }
  const int copies = 1 + (rnd(s, 1000) < s->dup);
  for(int c = 0; c < copies; c++) {
    qframe_t *q = NULL;
    for(int i = 0; i < Q_MAX; i++) {
      if(!soak_q[i].used) {
        q = &soak_q[i];
        break;
      }
    }
    if(q == NULL) {
      s->n_lost++;
      return;
    }
    q->used = 1;
    q->out = out;
    q->len = len;
    q->order = s->order++;
    q->at = clock_get() + s->delay;
    if(rnd(s, 1000) < s->reorder)
      q->at += rnd(s, 4 * s->delay + 2000);
    memcpy(q->data, f, len);
  }
}

static qframe_t *
link_next(void)
{
  qframe_t *best = NULL;
  for(int i = 0; i < Q_MAX; i++) {
    qframe_t *q = &soak_q[i];
    if(!q->used)
      continue;
    if(best == NULL || q->at < best->at ||
       (q->at == best->at && (int32_t)(q->order - best->order) < 0))
      best = q;
  }
  return best;
}

static void
soak_tx(sim_tcp_t *t, const uint8_t *f, size_t len)
{
  soak_t *s = (soak_t *)t;
  s->n_out++;
  if(soak_trace) {
    const uint8_t *th = f + 34;
    hosttest_log("  > seq=%u ack=%u len=%d flags=%02x wnd=%u",
                 (uint32_t)((th[4] << 24) | (th[5] << 16) | (th[6] << 8) |
                            th[7]) - (t->isn + 1),
                 (uint32_t)((th[8] << 24) | (th[9] << 16) | (th[10] << 8) |
                            th[11]) - (t->mios_isn + 1),
                 (int)(len - 34 - (th[12] >> 4) * 4), th[13],
                 (th[14] << 8) | th[15]);
  }
  link_put(s, 1, f, len);
}


// -- The endpoint --

// Window field for the next segment. The right edge never moves back:
// while the peer is "not reading" it stays, and the window runs out
static uint16_t
soak_wnd(soak_t *s)
{
  if(!s->closed && (int32_t)(s->rcv_nxt + s->wnd - s->edge) > 0)
    s->edge = s->rcv_nxt + s->wnd;

  uint32_t w = (int32_t)(s->edge - s->rcv_nxt) > 0 ? s->edge - s->rcv_nxt : 0;
  w = MIN(w >> s->ws, 65535);
  const uint32_t e = s->rcv_nxt + (w << s->ws);
  if((int32_t)(e - s->edge_adv) > 0)
    s->edge_adv = e;
  return w;
}

// What lies behind a hole, for the SACK option: the block the last
// segment went into first
static void
soak_sack_blocks(soak_t *s)
{
  sim_tcp_t *t = &s->t;
  t->nsack = 0;
  if(!s->sack || s->rx_done)
    return;

  uint32_t blk[16][2];
  int n = 0, last = -1;
  uint32_t i = s->rcv_nxt;
  while(i < s->rx_high && n < 16) {
    while(i < s->rx_high && !soak_rxmap[i])
      i++;
    if(i >= s->rx_high)
      break;
    blk[n][0] = i;
    while(i < s->rx_high && soak_rxmap[i])
      i++;
    blk[n][1] = i;
    if(s->rx_last >= blk[n][0] && s->rx_last < blk[n][1])
      last = n;
    n++;
  }
  if(last < 0)
    last = 0;
  for(int k = 0; k < n && t->nsack < 4; k++) {
    const int b = (last + k) % n;
    t->sack[t->nsack][0] = blk[b][0];
    t->sack[t->nsack][1] = blk[b][1];
    t->nsack++;
  }
}

static void
soak_send(soak_t *s, int flags, uint32_t seq, const void *data, size_t len)
{
  const uint16_t w = soak_wnd(s);
  soak_sack_blocks(s);
  sim_tcp_send(&s->t, flags | SIM_TCP_ACK, seq, s->rcv_nxt, w, data, len);
  s->delack_at = 0;
  s->unacked = 0;
}

static void
soak_send_ack(soak_t *s)
{
  soak_send(s, 0, MIN(s->snd_nxt, s->snd_max), NULL, 0);
}

static int
soak_fin_ok(const soak_t *s)
{
  return s->mode != MODE_SOURCE || s->early_fin || s->rx_done;
}

// Is there something Mios has not acked, or that is yet to be sent
static int
soak_tx_pending(const soak_t *s)
{
  return s->snd_una < s->tx_total ||
    (s->snd_una == s->tx_total && soak_fin_ok(s));
}

// Send the segment that begins at seq, at most max bytes. Returns the
// sequence number after it
static uint32_t
soak_send_segment(soak_t *s, uint32_t seq, uint32_t max)
{
  static uint8_t buf[1460];

  if(seq == s->tx_total) {
    soak_send(s, SIM_TCP_FIN, seq, NULL, 0);
    return seq + 1;
  }
  uint32_t len = MIN(MIN(s->tx_total - seq, max), s->mss);
  // Not what Mios said it has
  for(uint32_t i = 1; i < len; i++) {
    if(soak_txmap[seq + i]) {
      len = i;
      break;
    }
  }
  for(uint32_t i = 0; i < len; i++)
    buf[i] = pattern(seq + i);
  soak_send(s, SIM_TCP_PSH, seq, buf, len);
  return seq + len;
}

static void
soak_send_more(soak_t *s)
{
  while(1) {
    // After a timeout snd_nxt starts over: skip what Mios has
    while(s->snd_nxt < s->tx_total && soak_txmap[s->snd_nxt])
      s->snd_nxt++;

    const uint32_t in_flight = s->snd_nxt - s->snd_una;
    if(in_flight >= PEER_BURST * s->mss)
      break;

    if(s->snd_nxt < s->tx_total) {
      uint32_t room = s->mios_wnd > in_flight ? s->mios_wnd - in_flight : 0;
      if(!room) {
        // One byte into a closed window after a timeout, as a probe
        if(!s->probe || in_flight)
          break;
        room = 1;
      }
      s->probe = 0;
      if(s->seg_random && rnd(s, 4) == 0)
        room = 1 + rnd(s, MIN(room, s->mss));
      s->snd_nxt = soak_send_segment(s, s->snd_nxt, room);
    } else if(s->snd_nxt == s->tx_total && soak_fin_ok(s)) {
      s->snd_nxt = soak_send_segment(s, s->snd_nxt, 0);
    } else {
      break;
    }
    if(s->snd_nxt > s->snd_max)
      s->snd_max = s->snd_nxt;
  }
}

static void
soak_segment(soak_t *s, const sim_tcp_seg_t *seg)
{
  const uint64_t now = clock_get();
  s->n_in++;
  s->last_in = now;

  if(soak_trace) {
    hosttest_log("  < seq=%u ack=%u len=%d flags=%02x wnd=%u sack=%d %u-%u",
                 seg->seq, seg->ack, seg->len, seg->flags, seg->wnd,
                 seg->nsack, seg->nsack ? seg->sack[0][0] : 0,
                 seg->nsack ? seg->sack[0][1] : 0);
  }

  // (Once all is over a RST is the right answer to anything more that
  // reaches Mios, an ACK that was late for example)
  if(seg->flags & SIM_TCP_RST) {
    if(!(s->rx_done && s->snd_una == s->tx_total + 1))
      s->reset = 1;
    return;
  }

  if(seg->flags & SIM_TCP_SYN) {
    if(seg->flags != (SIM_TCP_SYN | SIM_TCP_ACK) || seg->ack != 0)
      return;
    if(s->established && seg->seq != s->mios_isn) {
      // A copy of our SYN that reached Mios after the connection was
      // over has started a new one. Refuse it
      s->t.mios_isn = s->mios_isn;
      sim_tcp_send(&s->t, SIM_TCP_RST, 0, 0, 0, NULL, 0);
      return;
    }
    if(!s->established) {
      s->established = 1;
      s->mios_isn = seg->seq;
      s->mios_wnd = seg->wnd;
      s->rx_mss = s->t.mss ? s->t.mss : 536;
      s->mss = MIN(seg->mss > 0 ? seg->mss : 536, s->rx_mss);
      if(s->t.wscale >= 0 && seg->wscale >= 0) {
        s->ws = s->t.wscale;
        s->mios_ws = seg->wscale;
      }
      SOAK_CHECK(s, s->t.wscale >= 0 || seg->wscale < 0,
                 "window scale option that was not offered");
      SOAK_CHECK(s, s->t.sack_permitted || !seg->sack_permitted,
                 "SACK-permitted option that was not offered");
      s->sack = s->t.sack_permitted && seg->sack_permitted;
    }
    soak_send_ack(s);
    return;
  }

  if(!s->established)
    return;

  // ---- What Mios acks ----

  SOAK_CHECK(s, (int32_t)(seg->ack - s->snd_max) <= 0,
             "ack %u is beyond what was sent (%u)", seg->ack, s->snd_max);

  const int32_t acked = seg->ack - s->snd_una;
  const uint32_t wnd = (uint32_t)seg->wnd << s->mios_ws;
  if(acked > 0) {
    s->snd_una = seg->ack;
    if(s->snd_nxt < s->snd_una)
      s->snd_nxt = s->snd_una;
    s->dupacks = 0;
    s->rto_at = now + PEER_RTO;
  } else if(acked == 0 && seg->len == 0 && !(seg->flags & SIM_TCP_FIN) &&
            s->snd_una != s->snd_max && wnd == s->mios_wnd) {
    if(++s->dupacks == 3) {
      // Resegmented, so it may reach past what was sent before
      const uint32_t e = soak_send_segment(s, s->snd_una, s->mss);
      if(e > s->snd_nxt)
        s->snd_nxt = e;
      if(e > s->snd_max)
        s->snd_max = e;
    }
  }
  if(acked >= 0)
    s->mios_wnd = wnd;

  SOAK_CHECK(s, s->sack || !seg->nsack, "SACK option without negotiation");
  for(int i = 0; i < seg->nsack; i++) {
    const uint32_t bs = seg->sack[i][0], be = seg->sack[i][1];
    const int ok = (int32_t)(be - bs) > 0 && (int32_t)(bs - seg->ack) > 0 &&
      (int32_t)(be - s->snd_max) <= 0 && be <= s->tx_total;
    SOAK_CHECK(s, ok, "SACK block %u-%u with ack=%u, sent up to %u",
               bs, be, seg->ack, s->snd_max);
    if(ok)
      memset(soak_txmap + bs, 1, be - bs);
  }

  // ---- What Mios sends ----

  const uint32_t seq = seg->seq;
  const uint32_t end = seq + seg->len;
  int ack_now = 0;

  if(seg->len) {
    SOAK_CHECK(s, seg->len <= s->rx_mss, "segment of %d bytes, mss is %d",
               seg->len, s->rx_mss);
    SOAK_CHECK(s, (int32_t)seq >= 0 && end <= s->rx_expect,
               "data at %u-%u, the stream is %u bytes", seq, end,
               s->rx_expect);
    SOAK_CHECK(s, (int32_t)(end - s->edge_adv) <= 0 ||
               (seg->len == 1 && end == s->edge_adv + 1),
               "data at %u-%u is outside the window, which ends at %u",
               seq, end, s->edge_adv);
    if((int32_t)seq < 0 || end > s->rx_expect)
      return;

    int bad = 0;
    for(int i = 0; i < seg->len; i++)
      bad += seg->data[i] != pattern(seq + i);
    SOAK_CHECK(s, !bad, "%d wrong bytes in segment at %u", bad, seq);

    if(seq != s->rcv_nxt)
      ack_now = 1;

    // Keep what is inside the window
    const uint32_t from = MAX(seq, s->rcv_nxt);
    const uint32_t to = (int32_t)(end - s->edge_adv) > 0 ? s->edge_adv : end;
    if(to > from) {
      memset(soak_rxmap + from, 1, to - from);
      s->rx_last = from;
      if(to > s->rx_high)
        s->rx_high = to;
    }
    while(s->rcv_nxt < s->rx_high && soak_rxmap[s->rcv_nxt])
      s->rcv_nxt++;

    if(!ack_now && ++s->unacked >= s->ack_every)
      ack_now = 1;
    if(!ack_now && !s->delack_at)
      s->delack_at = now + 40 * MSEC;
  } else if(seq != s->rcv_nxt) {
    ack_now = 1; // A keepalive
  }

  if(seg->flags & SIM_TCP_FIN) {
    SOAK_CHECK(s, end == s->rx_expect, "FIN at %u, the stream is %u bytes",
               end, s->rx_expect);
    s->rx_fin = 1;
    ack_now = 1;
  }
  if(s->rx_fin && !s->rx_done && s->rcv_nxt == s->rx_expect) {
    s->rx_done = 1;
    s->rcv_nxt++;
  }

  if(ack_now)
    soak_send_ack(s);
}


// Mios gives up on a connection it hears nothing from: after 5 s
// during the handshake, 21 s after that. On a link that loses enough
// that will happen, and it is not a fault. The peer can tell from when
// its frames got through
static uint64_t
soak_patience(const soak_t *s)
{
  return s->heard_ack ? 20900 * MSEC : 4900 * MSEC;
}

static void
soak_delivered(soak_t *s, int flags, uint64_t now)
{
  if(flags & SIM_TCP_SYN) {
    // Only the first SYN counts, Mios's clock runs from there
    if(!s->heard_at)
      s->heard_at = now;
    return;
  }
  if(s->heard_at && now - s->heard_at >= soak_patience(s))
    s->starved = 1;
  s->heard_at = now;
  s->heard_ack = 1;
}

static int
soak_starved(const soak_t *s)
{
  return s->starved ||
    (s->heard_at && clock_get() - s->heard_at >= soak_patience(s));
}


// Run the connection until 'deadline' or, if 'until_done', until both
// directions are finished. Returns 1 in the latter case
static int
soak_run(soak_t *s, uint64_t deadline, int until_done)
{
  static uint8_t frame[2048];
  sim_tcp_seg_t seg;

  while(1) {
    uint64_t now = clock_get();

    // Frames that are due
    qframe_t *q;
    while((q = link_next()) != NULL && q->at <= now) {
      q->used = 0;
      if(q->out) {
        if(soak_trace > 1)
          hosttest_log("  delivered flags=%02x", q->data[47]);
        soak_delivered(s, q->data[47], now);
        vnet_peer_send(soak_vnet, q->data, q->len);
      }
      else if(sim_tcp_input(&s->t, q->data, q->len, &seg))
        soak_segment(s, &seg);
    }

    if(until_done && s->rx_done && s->snd_una == s->tx_total + 1)
      return 1;
    if(now >= deadline || s->failed || s->reset)
      return 0;

    // With nothing to send the peer would wait for ever for a Mios
    // that has given up
    if(until_done && s->established && now >= s->last_in + 60 * SEC)
      return 0;

    uint64_t next = deadline;

    if(!s->established) {
      if(now >= s->syn_at) {
        sim_tcp_send(&s->t, SIM_TCP_SYN, -1, 0, MIN(s->wnd, 65535), NULL, 0);
        s->syn_at = now + PEER_RTO;
      }
      next = MIN(next, s->syn_at);
    } else {
      if(s->rto_at && now >= s->rto_at) {
        // Go back to the first byte that is not acked
        s->snd_nxt = s->snd_una;
        s->probe = 1;
        s->dupacks = 0;
        s->rto_at = 0;
      }
      if(s->stall && now >= s->stall_at) {
        s->closed = !s->closed;
        s->stall_at = now + (s->closed ? 100 * MSEC + rnd(s, 1500 * MSEC) :
                             rnd(s, 500 * MSEC));
        if(!s->closed)
          soak_send_ack(s); // The window update
      }
      if(s->delack_at && now >= s->delack_at)
        soak_send_ack(s);

      soak_send_more(s);

      if(!soak_tx_pending(s))
        s->rto_at = 0;
      else if(!s->rto_at)
        s->rto_at = now + PEER_RTO;

      if(s->rto_at)
        next = MIN(next, s->rto_at);
      if(s->delack_at)
        next = MIN(next, s->delack_at);
      if(s->stall)
        next = MIN(next, s->stall_at);
    }

    if((q = link_next()) != NULL)
      next = MIN(next, q->at);

    const ssize_t n = vnet_peer_recv(soak_vnet, frame, sizeof(frame), next);
    if(n < 14)
      continue;
    if(frame[12] == 0x08 && frame[13] == 0x00)
      link_put(s, 0, frame, n);
    else
      sim_tcp_input(&s->t, frame, n, &seg); // ARP is not impaired
  }
}


static int
pick(soak_t *s, const int *v, int n)
{
  return v[rnd(s, n)];
}

#define PICK(s, ...) ({                                                 \
      static const int v_[] = { __VA_ARGS__ };                          \
      pick(s, v_, sizeof(v_) / sizeof(v_[0])); })

// Returns 1 if all went well
static int
soak_connection(uint32_t seed, int conn)
{
  static const int ports[] = { PORT_SINK, PORT_SOURCE, PORT_ECHO };
  static const uint8_t mac[6] = { 0x02, 0xd0, 0x00, 0x00, 0x0a, 0x01 };

  soak_t *s = &soak;
  memset(s, 0, sizeof(soak_t));
  memset(soak_q, 0, sizeof(soak_q));
  memset(soak_txmap, 0, sizeof(soak_txmap));
  memset(soak_rxmap, 0, sizeof(soak_rxmap));

  s->conn = conn;
  s->rng = (seed * 2654435761u) ^ (conn * 40503u + 1);
  if(!s->rng)
    s->rng = 1;
  for(int i = 0; i < 8; i++)
    rnd(s, 2);

  s->mode = conn % 3;

  uint32_t bytes;
  switch(rnd(s, 4)) {
  case 0:
    bytes = rnd(s, 3000);
    break;
  case 1:
    bytes = STREAM_MAX / 2 + rnd(s, STREAM_MAX / 2);
    break;
  default:
    bytes = rnd(s, 40000);
    break;
  }
  s->tx_total = s->mode == MODE_SOURCE ? 0 : bytes;
  s->rx_expect = s->mode == MODE_SINK ? 0 : bytes;

  s->drop = PICK(s, 0, 0, 5, 20, 50, 150);
  s->dup = PICK(s, 0, 0, 10, 50);
  s->reorder = PICK(s, 0, 0, 20, 200);
  s->delay = PICK(s, 0, 500, 5000, 30000);
  s->wnd = PICK(s, 65535, 262144, 16384, 4096, 1000);
  s->seg_random = rnd(s, 2);
  s->ack_every = 1 + rnd(s, 2);
  s->early_fin = rnd(s, 2);
  s->stall = rnd(s, 4) == 0;
  s->stall_at = clock_get() + rnd(s, SEC);

  sim_tcp_t *t = &s->t;
  memcpy(t->mac, mac, 6);
  memset(t->mios_mac, 0xff, 6);
  t->vnet = soak_vnet;
  t->ip = peer_ip;
  t->mios_ip = mios_ip;
  t->port = 20000 + conn % 40000;
  t->mios_port = ports[s->mode];
  t->mss = PICK(s, 1460, 1460, 1460, 536, 200, 0);
  t->wscale = PICK(s, -1, 0, 2, 4);
  if(s->wnd > 65535 && t->wscale < 2)
    t->wscale = 2;
  t->sack_permitted = rnd(s, 4) != 0;
  // Now and then one that wraps during the connection
  t->isn = rnd(s, 4) ? xorshift(&s->rng) : -1 - rnd(s, bytes + 2);
  t->tx = soak_tx;

  app_seed = xorshift(&s->rng);
  app_source_bytes = s->rx_expect;
  app_slow = rnd(s, 4) == 0;
  app_opened = 0;
  app_rx = 0;
  app_bad = 0;
  app_err = 0;
  app_done = 0;

  const uint64_t t0 = clock_get();
  s->last_in = t0;
  const int done = soak_run(s, t0 + 1800 * SEC, 1);

  if(!done && !s->failed && soak_starved(s)) {
    // The link lost so much that Mios gave up, as it should. Make sure
    // nothing is left of the connection
    t->tx = NULL;
    t->nsack = 0;
    sim_tcp_send(t, SIM_TCP_RST, s->snd_una, 0, 0, NULL, 0);
    soak_run(s, clock_get() + SEC, 0);
    soak_aborted++;
    return 1;
  }

  SOAK_CHECK(s, done, "not finished after %d s: %s established=%d "
             "peer acked %u of %u, peer got %u of %u, fin=%d",
             (int)((clock_get() - t0) / SEC),
             s->reset ? "reset by Mios," : "", s->established,
             s->snd_una, s->tx_total + 1, s->rcv_nxt, s->rx_expect,
             s->rx_fin);
  if(s->failed)
    return 0;

  // Mios may send its FIN again if our ACK was lost. Stay around until
  // it has been quiet for longer than it lingers in TIME_WAIT
  const uint64_t t1 = clock_get();
  s->last_in = t1;
  s->stall = 0;
  while(clock_get() < s->last_in + 6 * SEC && clock_get() < t1 + 120 * SEC)
    soak_run(s, s->last_in + 6 * SEC, 0);

  SOAK_CHECK(s, app_done, "Mios's application did not finish");
  SOAK_CHECK(s, app_rx == s->tx_total, "Mios's application read %u of %u",
             app_rx, s->tx_total);
  SOAK_CHECK(s, !app_bad, "Mios's application read %d wrong bytes", app_bad);
  SOAK_CHECK(s, !app_err, "Mios's application could not write");

  // The connection must be gone now: anything more gets a RST. If the
  // link lost both our last ACK and what Mios sent to get another, Mios
  // may still be waiting for it. This ACK gets there for sure, and
  // then Mios is done after TIME_WAIT
  t->tx = NULL;
  t->nsack = 0;
  sim_tcp_seg_t seg;
  int gone = 0;
  for(int i = 0; i < 3 && !gone; i++) {
    sim_tcp_send(t, SIM_TCP_ACK, s->tx_total + 1, s->rcv_nxt, 0, NULL, 0);
    const uint64_t until = clock_get() + 6 * SEC;
    while(!gone && clock_get() < until &&
          sim_tcp_recv(t, &seg, until - clock_get()))
      gone = !!(seg.flags & SIM_TCP_RST);
  }
  SOAK_CHECK(s, gone, "the connection is still there on Mios's side");

  if(host_arg("verbose") != NULL) {
    soak_describe(s);
    hosttest_log("  %d.%03d s, %d in, %d out, %d lost",
                 (int)((t1 - t0) / SEC), (int)((t1 - t0) / MSEC % 1000),
                 s->n_in, s->n_out, s->n_lost);
  }
  return !s->failed;
}


static uint32_t soak_seed;
static int soak_rounds;
static int soak_only = -1;
static volatile int soak_completed;

static void
script_thread(void *arg)
{
  if(soak_only >= 0) {
    soak_connection(soak_seed, soak_only);
    soak_completed = 1;
  } else {
    for(int i = 0; i < soak_rounds; i++) {
      if(!soak_connection(soak_seed, i))
        break;
      soak_completed++;
    }
  }
  script_done = 1;
  while(1)
    sim_wait(SIM_NEVER);
}


// ---- The suite (Mios main thread) ----

static uint32_t
ip4(int a, int b, int c, int d)
{
  const uint8_t v[4] = { a, b, c, d };
  uint32_t r;
  memcpy(&r, v, 4);
  return r;
}

static int
run_tcp_soak(void)
{
  const char *arg;

  soak_seed = (arg = host_arg("seed")) != NULL && *arg ? atoi(arg) : 1;
  soak_rounds = (arg = host_arg("rounds")) != NULL && *arg ? atoi(arg) : 300;
  if((arg = host_arg("conn")) != NULL && *arg)
    soak_only = atoi(arg);
  if((arg = host_arg("trace")) != NULL)
    soak_trace = *arg ? atoi(arg) : 1;

  // A static address and no link-up event, so no DHCP client
  const uint8_t mac[6] = { 0x02, 0xc0, 0x00, 0x00, 0x0a, 0x02 };
  soak_vnet = vnet_create("veth10", mac);
  netif_t *ni = &vnet_eni(soak_vnet)->eni_ni;
  mios_ip = ip4(10, 10, 0, 2);
  peer_ip = ip4(10, 10, 0, 1);
  ni->ni_ipv4_local_addr = mios_ip;
  ni->ni_ipv4_local_prefixlen = 24;

  sim_thread_create("tcp-soak", script_thread, NULL, 0);

  while(!script_done)
    usleep(100000);

  hosttest_log("%d connections, %d of them given up on by Mios when "
               "the link lost too much", soak_completed, soak_aborted);
  CHECK(soak_aborted * 50 <= soak_completed + 50,
        "too many connections given up on");
  return 0; // failures are counted by CHECK()
}

HOSTTEST_SUITE("tcp-soak", run_tcp_soak, 0);
