/*
 * tcp: src/net/ipv4/tcp.c against a scripted peer (sim_tcp.h), in
 * virtual time.
 *
 * The scenarios run in a simulation thread that plays the remote host
 * segment by segment, so it decides exactly what is lost and when, and
 * sees when Mios reacts. The Mios end of each connection is one of the
 * small services below.
 */

#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/param.h>

#include <mios/service.h>
#include <mios/stream.h>
#include <mios/task.h>

#include <net/ether.h>
#include <net/ipv4/tcp.h>

#include "hosttest.h"
#include "sim.h"
#include "sim_tcp.h"

#define MSEC 1000ull
#define SEC  1000000ull

#define MSS 1460

#define PORT_SOURCE 7000
#define PORT_SINK   7001
#define PORT_SINK8K 7002

// Byte at stream offset 'off' of everything the test sends, either way
static uint8_t
pattern(uint32_t off)
{
  return off ^ ((off >> 8) * 31) ^ ((off >> 16) * 7);
}


// ---- Mios side ----

/* Source: the peer sends commands, 'W' and a 32 bit big endian count
   makes the service write that many pattern bytes. The pattern offset
   runs on over the connection. */

static void *
source_thread(void *arg)
{
  // One write per chunk of 32 kB: a write that fits the TX FIFO goes
  // out as full segments. One connection at a time
  static uint8_t buf[32768];
  stream_t *s = arg;
  uint32_t off = 0;
  uint8_t cmd[5];

  while(stream_read(s, cmd, sizeof(cmd), sizeof(cmd)) == sizeof(cmd)) {
    if(cmd[0] != 'W')
      break;
    uint32_t n = (cmd[1] << 24) | (cmd[2] << 16) | (cmd[3] << 8) | cmd[4];
    while(n) {
      const size_t chunk = MIN(n, sizeof(buf));
      for(size_t i = 0; i < chunk; i++)
        buf[i] = pattern(off + i);
      if(stream_write(s, buf, chunk, 0) != chunk)
        break;
      off += chunk;
      n -= chunk;
    }
    stream_write(s, NULL, 0, 0); // Flush
  }
  stream_close(s);
  return NULL;
}

static error_t
source_open(stream_t *s)
{
  return thread_create(source_thread, s, 16384, "tcpsource",
                       TASK_DETACHED, 5) ? 0 : ERR_NO_MEMORY;
}

SERVICE_DEF_STREAM_EX("tcptest-source", PORT_SOURCE, 15, 11, source_open);


/* Sink: stores what the peer sends, for the script to look at. One
   connection at a time. While sink_pause is set it does not read, so
   the RX FIFO (2 kB) fills up. */

static uint8_t sink_buf[65536];
static volatile uint32_t sink_len;
static volatile int sink_eof;
static volatile int sink_pause;

static void *
sink_thread(void *arg)
{
  stream_t *s = arg;
  while(1) {
    while(sink_pause)
      usleep(1000);
    const ssize_t n = stream_read(s, sink_buf + sink_len,
                                  sizeof(sink_buf) - sink_len, 1);
    if(n <= 0)
      break;
    sink_len += n;
  }
  sink_eof = 1;
  stream_close(s);
  return NULL;
}

static error_t
sink_open(stream_t *s)
{
  sink_len = 0;
  sink_eof = 0;
  return thread_create(sink_thread, s, 16384, "tcpsink",
                       TASK_DETACHED, 5) ? 0 : ERR_NO_MEMORY;
}

SERVICE_DEF_STREAM("tcptest-sink", PORT_SINK, sink_open);

// The same with an 8 kB RX FIFO
SERVICE_DEF_STREAM_EX("tcptest-sink8k", PORT_SINK8K, 12, 13, sink_open);


/* Client: a connection opened from Mios. The script asks for what it
   wants done through client_op, and the suite's main thread, which is
   allowed to call into Mios, does it. */

#define CLIENT_CONNECT 1  // arg: the peer's port
#define CLIENT_WRITE   2  // arg: number of pattern bytes
#define CLIENT_CLOSE   3

static volatile int client_op;
static volatile uint32_t client_arg;


// ---- Peer side: helpers for the scenarios ----

static vnet_t *tcp_vnet;
static uint32_t peer_ip, mios_ip;
static volatile int script_done;

static sim_tcp_t *
peer_new(int mios_port)
{
  static int next_port = 40000;
  sim_tcp_t *t = calloc(1, sizeof(sim_tcp_t));
  const uint8_t mac[6] = { 0x02, 0xd0, 0x00, 0x00, 0x09, 0x01 };
  memcpy(t->mac, mac, 6);
  t->vnet = tcp_vnet;
  t->ip = peer_ip;
  t->mios_ip = mios_ip;
  t->port = next_port++;
  t->mios_port = mios_port;
  t->mss = MSS;
  t->wscale = -1;
  t->isn = 1000000 * (next_port - 40000);
  return t;
}

static int
data_ok(const sim_tcp_seg_t *seg)
{
  for(int i = 0; i < seg->len; i++) {
    if(seg->data[i] != pattern(seg->seq + i))
      return 0;
  }
  return 1;
}

// Ask the source service for n bytes. The command is the peer's data
// at *cmdseq
static void
source_request(sim_tcp_t *t, uint32_t *cmdseq, uint32_t ack, uint32_t n)
{
  const uint8_t cmd[5] = { 'W', n >> 24, n >> 16, n >> 8, n };
  sim_tcp_send(t, SIM_TCP_ACK | SIM_TCP_PSH, *cmdseq, ack, 65535,
               cmd, sizeof(cmd));
  *cmdseq += sizeof(cmd);
}

// Receive in-order data up to stream offset 'end', acking every
// segment. Returns 1 when it all arrived
static int
drain(sim_tcp_t *t, uint32_t *rcv_nxt, uint32_t end, uint32_t cmdseq)
{
  sim_tcp_seg_t seg;
  while(*rcv_nxt != end) {
    if(!sim_tcp_recv(t, &seg, 2 * SEC))
      return 0;
    if(!seg.len)
      continue;
    if(seg.seq != *rcv_nxt || !data_ok(&seg))
      return 0;
    *rcv_nxt += seg.len;
    sim_tcp_send(t, SIM_TCP_ACK, cmdseq, *rcv_nxt, 65535, NULL, 0);
  }
  return 1;
}

// Receive data segments without acking them until stream offset 'end'
// has been sent. Returns the number of segments, -1 on anything odd
static int
collect(sim_tcp_t *t, uint32_t from, uint32_t end)
{
  sim_tcp_seg_t seg;
  int n = 0;
  while(from != end) {
    if(!sim_tcp_recv(t, &seg, 2 * SEC))
      return -1;
    if(!seg.len)
      continue;
    if(seg.seq != from || !data_ok(&seg))
      return -1;
    from += seg.len;
    n++;
  }
  return n;
}

// Have the main thread do a client operation. It notices within a ms
static void
client(int op, uint32_t arg)
{
  client_arg = arg;
  client_op = op;
}

static void
client_wait(void)
{
  while(client_op)
    sim_wait(clock_get() + 100);
}

// Peer closes, Mios closes back
static void
peer_close(sim_tcp_t *t, uint32_t seq, uint32_t ack)
{
  sim_tcp_seg_t seg;
  int got_fin = 0;
  sim_tcp_send(t, SIM_TCP_FIN | SIM_TCP_ACK, seq, ack, 65535, NULL, 0);
  while(!got_fin && sim_tcp_recv(t, &seg, SEC)) {
    if(seg.flags & SIM_TCP_FIN) {
      got_fin = 1;
      CHECK(seg.ack == seq + 1, "FIN not acked: ack=%u", seg.ack);
      sim_tcp_send(t, SIM_TCP_ACK, seq + 1, seg.seq + seg.len + 1, 65535,
                   NULL, 0);
    }
  }
  CHECK(got_fin, "no FIN from Mios");
}


// ---- Scenarios ----

static void
scn_transfer(void)
{
  hosttest_log("---- handshake, data from Mios, close ----");
  sim_tcp_t *t = peer_new(PORT_SOURCE);
  sim_tcp_seg_t synack;
  uint32_t cmdseq = 0, rcv_nxt = 0;

  if(!CHECK(sim_tcp_connect(t, 65535, &synack), "handshake failed"))
    return;
  CHECK(synack.mss == MSS, "SYN-ACK mss=%d", synack.mss);
  CHECK(synack.wscale == -1, "SYN-ACK has a window scale we never offered");
  CHECK(!synack.sack_permitted, "SYN-ACK has a SACK-permitted we never sent");

  source_request(t, &cmdseq, 0, 3000);
  CHECK(drain(t, &rcv_nxt, 3000, cmdseq), "data stopped at %u", rcv_nxt);

  source_request(t, &cmdseq, rcv_nxt, 100000);
  CHECK(drain(t, &rcv_nxt, 103000, cmdseq), "data stopped at %u", rcv_nxt);

  peer_close(t, cmdseq, rcv_nxt);
}


static void
scn_sink(void)
{
  hosttest_log("---- data to Mios ----");
  sim_tcp_t *t = peer_new(PORT_SINK);
  sim_tcp_seg_t seg;
  static uint8_t data[5000];
  for(size_t i = 0; i < sizeof(data); i++)
    data[i] = pattern(i);

  if(!CHECK(sim_tcp_connect(t, 65535, NULL), "handshake failed"))
    return;

  // One segment at a time, the sink's FIFO is only 2 kB
  uint32_t seq = 0;
  while(seq < sizeof(data)) {
    const size_t len = MIN(sizeof(data) - seq, 1000);
    sim_tcp_send(t, SIM_TCP_ACK | SIM_TCP_PSH, seq, 0, 65535, data + seq, len);
    seq += len;
    uint32_t acked = 0;
    while(acked != seq && sim_tcp_recv(t, &seg, 500 * MSEC))
      acked = seg.ack;
    if(!CHECK(acked == seq, "acked %u of %u", acked, seq))
      return;
  }
  CHECK(sink_len == sizeof(data), "sink has %u bytes", sink_len);
  CHECK(!memcmp(sink_buf, data, sizeof(data)), "sink data differs");

  peer_close(t, seq, 0);
  CHECK(sink_eof, "sink did not see the close");
}


static void
scn_rto(void)
{
  hosttest_log("---- retransmission timeout ----");
  sim_tcp_t *t = peer_new(PORT_SOURCE);
  sim_tcp_seg_t seg;
  uint32_t cmdseq = 0;

  if(!CHECK(sim_tcp_connect(t, 65535, NULL), "handshake failed"))
    return;

  source_request(t, &cmdseq, 0, 4 * MSS);
  const uint64_t t0 = clock_get();
  CHECK(collect(t, 0, 4 * MSS) == 4, "expected four full segments");

  // Nothing is acked. First the last segment comes again, as a tail
  // loss probe. That is not answered either, and the first segment
  // comes again after the RTO
  if(CHECK(sim_tcp_recv(t, &seg, SEC), "no probe")) {
    const int ms = (seg.at - t0) / 1000;
    CHECK(seg.seq == 3 * MSS && seg.len == MSS && data_ok(&seg),
          "probe seq=%u len=%d", seg.seq, seg.len);
    CHECK(ms >= 40 && ms <= 60, "probe after %d ms", ms);
  }
  if(CHECK(sim_tcp_recv(t, &seg, SEC), "no retransmission")) {
    const int ms = (seg.at - t0) / 1000;
    CHECK(seg.seq == 0 && seg.len == MSS && data_ok(&seg),
          "retransmitted seq=%u len=%d", seg.seq, seg.len);
    CHECK(ms >= 250 && ms <= 350, "retransmitted after %d ms", ms);
  }

  // Ack everything: quiet from then on
  sim_tcp_send(t, SIM_TCP_ACK, cmdseq, 4 * MSS, 65535, NULL, 0);
  CHECK(!sim_tcp_recv(t, &seg, 2 * SEC), "segment after everything was acked: "
        "seq=%u len=%d flags=%x", seg.seq, seg.len, seg.flags);

  peer_close(t, cmdseq, 4 * MSS);
}


// A pure ACK from the peer
static void
ack(sim_tcp_t *t, uint32_t cmdseq, uint32_t ackno, uint16_t wnd)
{
  sim_tcp_send(t, SIM_TCP_ACK, cmdseq, ackno, wnd, NULL, 0);
}

// Expect a retransmission of the full segment at 'seq' no later than
// 'within' us from now
static void
expect_rtx(sim_tcp_t *t, uint32_t seq, uint64_t within, const char *what)
{
  sim_tcp_seg_t seg;
  if(!CHECK(sim_tcp_recv(t, &seg, within), "%s: no retransmission of %u",
            what, seq))
    return;
  CHECK(seg.seq == seq && seg.len == MSS && data_ok(&seg),
        "%s: got seq=%u len=%d, expected seq=%u", what, seg.seq, seg.len, seq);
}

static void
expect_quiet(sim_tcp_t *t, uint64_t duration, const char *what)
{
  sim_tcp_seg_t seg = {};
  CHECK(!sim_tcp_recv(t, &seg, duration), "%s: unexpected seq=%u len=%d "
        "flags=%x", what, seg.seq, seg.len, seg.flags);
}


static void
scn_fast_retransmit(void)
{
  hosttest_log("---- fast retransmit ----");
  sim_tcp_t *t = peer_new(PORT_SOURCE);
  uint32_t cmdseq = 0;

  if(!CHECK(sim_tcp_connect(t, 65535, NULL), "handshake failed"))
    return;

  // Ten segments, of which the third "is lost"
  source_request(t, &cmdseq, 0, 10 * MSS);
  CHECK(collect(t, 0, 10 * MSS) == 10, "expected ten full segments");
  ack(t, cmdseq, 2 * MSS, 65535);

  // ACKs that change the window are window updates, not duplicates
  ack(t, cmdseq, 2 * MSS, 65000);
  ack(t, cmdseq, 2 * MSS, 64000);
  ack(t, cmdseq, 2 * MSS, 63000);
  expect_quiet(t, 10 * MSEC, "window updates");

  // Segments four to ten arrive at the peer: seven duplicate ACKs. The
  // third triggers the retransmission, right away
  ack(t, cmdseq, 2 * MSS, 63000);
  ack(t, cmdseq, 2 * MSS, 63000);
  expect_quiet(t, 10 * MSEC, "two duplicate ACKs");
  ack(t, cmdseq, 2 * MSS, 63000);
  expect_rtx(t, 2 * MSS, 1 * MSEC, "third duplicate ACK");
  for(int i = 0; i < 4; i++)
    ack(t, cmdseq, 2 * MSS, 63000);
  expect_quiet(t, 10 * MSEC, "further duplicate ACKs");

  ack(t, cmdseq, 10 * MSS, 63000);
  expect_quiet(t, 2 * SEC, "all acked");

  // The next loss on the same connection is recovered the same way
  source_request(t, &cmdseq, 10 * MSS, 5 * MSS);
  CHECK(collect(t, 10 * MSS, 15 * MSS) == 5, "expected five full segments");
  for(int i = 0; i < 3; i++)
    ack(t, cmdseq, 10 * MSS, 65535); // The request's window
  expect_rtx(t, 10 * MSS, 1 * MSEC, "second loss");
  ack(t, cmdseq, 15 * MSS, 65535);
  expect_quiet(t, 2 * SEC, "all acked again");

  peer_close(t, cmdseq, 15 * MSS);
}


static void
scn_multiple_losses(void)
{
  hosttest_log("---- several losses in one window ----");
  sim_tcp_t *t = peer_new(PORT_SOURCE);
  uint32_t cmdseq = 0;

  if(!CHECK(sim_tcp_connect(t, 65535, NULL), "handshake failed"))
    return;

  // Ten segments; the third, sixth and ninth are lost
  source_request(t, &cmdseq, 0, 10 * MSS);
  CHECK(collect(t, 0, 10 * MSS) == 10, "expected ten full segments");
  const uint64_t t0 = clock_get();
  ack(t, cmdseq, 2 * MSS, 65535);
  for(int i = 0; i < 3; i++)
    ack(t, cmdseq, 2 * MSS, 65535);
  expect_rtx(t, 2 * MSS, 1 * MSEC, "first hole");

  // Each retransmission fills one hole: the ACK stops at the next one
  // (a partial ACK), which must be retransmitted without a timeout
  ack(t, cmdseq, 5 * MSS, 65535);
  expect_rtx(t, 5 * MSS, 1 * MSEC, "second hole");
  ack(t, cmdseq, 8 * MSS, 65535);
  expect_rtx(t, 8 * MSS, 1 * MSEC, "third hole");
  ack(t, cmdseq, 10 * MSS, 65535);
  const int ms = (clock_get() - t0) / 1000;
  CHECK(ms < 10, "recovery took %d ms", ms);
  expect_quiet(t, 2 * SEC, "all acked");

  peer_close(t, cmdseq, 10 * MSS);
}


static void
scn_timeout_recovery(void)
{
  hosttest_log("---- several losses, found by timeout ----");
  sim_tcp_t *t = peer_new(PORT_SOURCE);
  uint32_t cmdseq = 0;

  if(!CHECK(sim_tcp_connect(t, 65535, NULL), "handshake failed"))
    return;

  // The whole burst of four is lost, and the tail loss probe after
  // it, so there are no duplicate ACKs. The timeout finds the first;
  // the rest follow on the partial ACKs
  source_request(t, &cmdseq, 0, 4 * MSS);
  CHECK(collect(t, 0, 4 * MSS) == 4, "expected four full segments");
  expect_rtx(t, 3 * MSS, 60 * MSEC, "probe");
  expect_rtx(t, 0, 300 * MSEC, "timeout");
  const uint64_t t0 = clock_get();
  for(int i = 1; i < 4; i++) {
    ack(t, cmdseq, i * MSS, 65535);
    expect_rtx(t, i * MSS, 1 * MSEC, "after timeout");
  }
  ack(t, cmdseq, 4 * MSS, 65535);
  const int ms = (clock_get() - t0) / 1000;
  CHECK(ms < 10, "recovery took %d ms", ms);
  expect_quiet(t, 2 * SEC, "all acked");

  peer_close(t, cmdseq, 4 * MSS);
}


static void
scn_active_open(void)
{
  hosttest_log("---- connection opened by Mios: window scale ----");
  sim_tcp_t *t = peer_new(0);
  sim_tcp_seg_t syn, seg;
  uint32_t rcv_nxt = 0;

  // The peer scales its window by 2^7, like Linux with default buffers
  t->wscale = 7;
  client(CLIENT_CONNECT, t->port);
  if(!CHECK(sim_tcp_accept(t, 8192, &syn, SEC), "handshake failed"))
    return;
  CHECK(syn.mss == MSS, "SYN mss=%d", syn.mss);
  CHECK(syn.wscale == 0, "SYN wscale=%d", syn.wscale);

  // 512 << 7 is 64 kB. Read unscaled it would be a third of a segment
  ack(t, 0, 0, 512);
  client_wait();
  client(CLIENT_WRITE, 20 * MSS);
  CHECK(collect(t, 0, 20 * MSS) == 20, "window not filled");
  ack(t, 0, 20 * MSS, 512);

  client_wait();
  client(CLIENT_CLOSE, 0);
  if(CHECK(sim_tcp_recv(t, &seg, SEC) && (seg.flags & SIM_TCP_FIN),
           "no FIN from Mios")) {
    sim_tcp_send(t, SIM_TCP_FIN | SIM_TCP_ACK, 0, 20 * MSS + 1, 512, NULL, 0);
    CHECK(sim_tcp_recv(t, &seg, SEC) && seg.ack == 1, "FIN not acked");
  }
  client_wait();


  hosttest_log("---- connection opened by Mios: no window scale, small MSS ----");
  t = peer_new(0);
  t->mss = 1000;
  client(CLIENT_CONNECT, t->port);
  if(!CHECK(sim_tcp_accept(t, 8192, &syn, SEC), "handshake failed"))
    return;

  // No window scale in the SYN-ACK: 3000 means 3000, three segments of
  // the peer's MSS
  ack(t, 0, 0, 3000);
  client_wait();
  client(CLIENT_WRITE, 10000);
  for(int i = 0; i < 3; i++) {
    if(CHECK(sim_tcp_recv(t, &seg, SEC), "no segment %d", i))
      CHECK(seg.seq == rcv_nxt && seg.len == 1000 && data_ok(&seg),
            "segment %d: seq=%u len=%d", i, seg.seq, seg.len);
    rcv_nxt += 1000;
  }
  expect_quiet(t, 40 * MSEC, "window full");

  sim_tcp_send(t, SIM_TCP_RST, 0, 0, 0, NULL, 0);
  client_wait();
  client(CLIENT_CLOSE, 0);
  client_wait();
}


static void
scn_write_before_established(void)
{
  hosttest_log("---- data written before the handshake is done, then close ----");
  sim_tcp_t *t = peer_new(0);
  sim_tcp_seg_t seg;
  uint32_t rcv_nxt = 0;
  const uint32_t total = 30000;

  // Most of a TX FIFO waits for the SYN-ACK, and the stream is closed
  // behind it: all of it must arrive, in order, and then the FIN
  t->wscale = 7;
  client(CLIENT_CONNECT, t->port);
  client_wait();
  client(CLIENT_WRITE, total);
  client_wait();
  client(CLIENT_CLOSE, 0);
  if(!CHECK(sim_tcp_accept_syn(t, 65535, NULL, SEC), "no SYN"))
    return;

  int got_fin = 0;
  while(!got_fin && sim_tcp_recv(t, &seg, SEC)) {
    if(seg.len) {
      if(!CHECK(seg.seq == rcv_nxt && data_ok(&seg), "seq=%u len=%d, "
                "expected seq=%u", seg.seq, seg.len, rcv_nxt))
        break;
      rcv_nxt += seg.len;
    }
    if(seg.flags & SIM_TCP_FIN) {
      got_fin = 1;
      CHECK(seg.seq + seg.len == total, "FIN at %u", seg.seq + seg.len);
    }
    sim_tcp_send(t, SIM_TCP_ACK, 0, rcv_nxt + got_fin, 512, NULL, 0);
  }
  CHECK(got_fin && rcv_nxt == total, "got %u bytes, FIN:%d", rcv_nxt, got_fin);
  sim_tcp_send(t, SIM_TCP_FIN | SIM_TCP_ACK, 0, total + 1, 512, NULL, 0);
  CHECK(sim_tcp_recv(t, &seg, SEC) && seg.ack == 1, "FIN not acked");
  client_wait();
}


// Send pattern bytes [from, to) to the sink
static void
sink_send(sim_tcp_t *t, uint32_t from, uint32_t to)
{
  static uint8_t data[MSS];
  for(uint32_t i = from; i < to; i++)
    data[i - from] = pattern(i);
  sim_tcp_send(t, SIM_TCP_ACK | SIM_TCP_PSH, from, 0, 65535, data, to - from);
}

// Expect an ACK for exactly 'ackno' no later than 'within' us from
// now. Returns its window, -1 if it did not come
static int
expect_ack(sim_tcp_t *t, uint32_t ackno, uint64_t within, const char *what)
{
  sim_tcp_seg_t seg;
  do {
    if(!CHECK(sim_tcp_recv(t, &seg, within), "%s: no ACK", what))
      return -1;
  } while(seg.seq == (uint32_t)-1); // Mios's own keepalive probe
  CHECK(seg.flags == SIM_TCP_ACK && seg.len == 0 && seg.ack == ackno,
        "%s: flags=%x len=%d ack=%u, expected ack=%u", what, seg.flags,
        seg.len, seg.ack, ackno);
  return seg.wnd;
}

static int
sink_ok(uint32_t len)
{
  // Give the sink thread a moment to read
  sim_wait(clock_get() + 5 * MSEC);
  if(sink_len != len)
    return 0;
  for(uint32_t i = 0; i < len; i++) {
    if(sink_buf[i] != pattern(i))
      return 0;
  }
  return 1;
}


static void
scn_rx_overlap(void)
{
  hosttest_log("---- receive: retransmissions, overlap, out of order ----");
  sim_tcp_t *t = peer_new(PORT_SINK);

  if(!CHECK(sim_tcp_connect(t, 65535, NULL), "handshake failed"))
    return;

  sink_send(t, 0, 500);
  expect_ack(t, 500, 50 * MSEC, "first segment");

  // A retransmission that starts in data we have and goes on into new
  // data: the new part must be taken
  sink_send(t, 200, 900);
  expect_ack(t, 900, 50 * MSEC, "overlapping segment");
  CHECK(sink_ok(900), "sink has %u bytes after overlap", sink_len);

  // Old data only: nothing to take, but every copy gets an ACK (the
  // peer retransmits because it has not seen one)
  for(int i = 0; i < 20; i++) {
    sink_send(t, 0, 500);
    expect_ack(t, 900, 1 * MSEC, "old duplicate");
    sim_wait(clock_get() + 50 * MSEC);
  }

  // A segment behind a hole is answered at once with a duplicate ACK
  // so the peer can fast retransmit, and kept: when the hole is
  // filled the ACK covers both, again at once
  sink_send(t, 1400, 1500);
  expect_ack(t, 900, 1 * MSEC, "segment behind a hole");
  CHECK(sink_ok(900), "data behind a hole was delivered");
  sink_send(t, 900, 1400);
  expect_ack(t, 1500, 1 * MSEC, "the hole");
  CHECK(sink_ok(1500), "sink has %u bytes at the end", sink_len);

  // Keepalive probes, Linux style (no data, one before what we expect)
  // and BSD style (one old byte): each one is answered
  for(int i = 0; i < 10; i++) {
    sim_tcp_send(t, SIM_TCP_ACK, 1499, 0, 65535, NULL, 0);
    expect_ack(t, 1500, 1 * MSEC, "keepalive probe");
    sim_wait(clock_get() + SEC);
  }
  sink_send(t, 1499, 1500);
  expect_ack(t, 1500, 1 * MSEC, "keepalive probe with a byte");

  // A burst of old duplicates gets some ACKs, not one each
  int acks = 0;
  sim_tcp_seg_t seg;
  for(int i = 0; i < 30; i++)
    sink_send(t, 0, 500);
  while(sim_tcp_recv(t, &seg, 1 * MSEC))
    acks++;
  CHECK(acks >= 3 && acks <= 10, "%d ACKs for a burst of 30 duplicates", acks);

  // A RST that is not exactly at rcv.nxt does not reset
  sim_tcp_send(t, SIM_TCP_RST, 1600, 0, 0, NULL, 0);
  sim_wait(clock_get() + 20 * MSEC);
  CHECK(!sink_eof, "RST in the window closed the connection");
  sim_tcp_send(t, SIM_TCP_RST, 1500, 0, 0, NULL, 0);
  sim_wait(clock_get() + 20 * MSEC);
  CHECK(sink_eof, "RST at rcv.nxt did not close the connection");
}


static void
scn_rx_window(void)
{
  hosttest_log("---- receive: full window, probes, window update ----");
  sim_tcp_t *t = peer_new(PORT_SINK);
  sim_tcp_seg_t seg;

  // The sink does not read. Its FIFO holds 2048 bytes
  sink_pause = 1;
  if(!CHECK(sim_tcp_connect(t, 65535, NULL), "handshake failed"))
    return;

  sink_send(t, 0, MSS);
  CHECK(expect_ack(t, MSS, 50 * MSEC, "first segment") == 2048 - MSS,
        "window after the first segment");

  // More than fits: what fits is taken
  sink_send(t, MSS, MSS + 1000);
  CHECK(expect_ack(t, 2048, 50 * MSEC, "segment over the window") == 0,
        "window not closed");

  // Zero window probes are answered, every one of them
  for(int i = 0; i < 10; i++) {
    sink_send(t, 2048, 2049);
    CHECK(expect_ack(t, 2048, 1 * MSEC, "zero window probe") == 0,
          "window open during probes");
    sim_wait(clock_get() + 200 * MSEC);
  }

  // The application reads again: Mios says so by itself
  sink_pause = 0;
  if(CHECK(sim_tcp_recv(t, &seg, 50 * MSEC), "no window update")) {
    CHECK(seg.ack == 2048 && seg.wnd >= 1024, "window update: ack=%u wnd=%u",
          seg.ack, seg.wnd);
  }
  // (The sink is waiting in a read now and stops after this one)
  sink_pause = 1;
  sink_send(t, 2048, MSS + 1000);
  expect_ack(t, MSS + 1000, 50 * MSEC, "rest of the segment");
  CHECK(sink_ok(MSS + 1000), "sink has %u bytes", sink_len);

  // The last data and the FIN are both waiting when the application
  // reads: it must get the data before it sees the end
  sink_send(t, MSS + 1000, MSS + 1500);
  expect_ack(t, MSS + 1500, 50 * MSEC, "last data");
  sim_tcp_send(t, SIM_TCP_FIN | SIM_TCP_ACK, MSS + 1500, 0, 65535, NULL, 0);
  expect_ack(t, MSS + 1501, 50 * MSEC, "FIN");
  sink_pause = 0;
  CHECK(sink_ok(MSS + 1500), "sink has %u bytes at the end", sink_len);
  CHECK(sink_eof, "sink did not see the close");
  if(CHECK(sim_tcp_recv(t, &seg, SEC) && (seg.flags & SIM_TCP_FIN),
           "no FIN from Mios"))
    sim_tcp_send(t, SIM_TCP_ACK, MSS + 1501, 1, 65535, NULL, 0);
}


static void
scn_dup_ack_window(void)
{
  hosttest_log("---- receive: duplicate ACKs repeat the window ----");
  sim_tcp_t *t = peer_new(PORT_SINK8K);

  sink_pause = 1;
  if(!CHECK(sim_tcp_connect(t, 65535, NULL), "handshake failed"))
    return;

  // 1000 bytes sit in the FIFO when they are acked, then the
  // application reads them. The window is now bigger than the last
  // one we advertised, but not by enough for a window update
  sink_send(t, 0, 1000);
  const int wnd = expect_ack(t, 1000, 50 * MSEC, "first segment");
  CHECK(wnd == 8192 - 1000, "window %d", wnd);
  sink_pause = 0;
  expect_quiet(t, 50 * MSEC, "after the application read");

  // Segments behind a hole: the ACKs must be exact duplicates of the
  // last one, window included. A peer does not count an ACK with a
  // changed window as a duplicate (RFC 5681 section 2) and would
  // never fast retransmit
  for(int i = 0; i < 3; i++) {
    sink_send(t, 2000, 3000);
    CHECK(expect_ack(t, 1000, 1 * MSEC, "segment behind a hole") == wnd,
          "duplicate ACK with another window");
  }

  // The ACK for the retransmission has the real window: 2000 bytes
  // have just gone into an empty FIFO
  sink_send(t, 1000, 2000);
  CHECK(expect_ack(t, 3000, 1 * MSEC, "the hole") == 8192 - 2000,
        "window after the hole was filled");
  CHECK(sink_ok(3000), "sink has %u bytes", sink_len);

  peer_close(t, 3000, 0);
}


static void
scn_out_of_order(void)
{
  hosttest_log("---- receive: several holes ----");
  sim_tcp_t *t = peer_new(PORT_SINK8K);
  sim_tcp_seg_t seg;

  if(!CHECK(sim_tcp_connect(t, 65535, NULL), "handshake failed"))
    return;

  // Three separate blocks behind the first hole, arriving in no
  // particular order and partly overlapping
  sink_send(t, 3000, 3200);
  expect_ack(t, 0, 1 * MSEC, "block 3");
  sink_send(t, 1000, 1200);
  expect_ack(t, 0, 1 * MSEC, "block 1");
  sink_send(t, 2000, 2200);
  expect_ack(t, 0, 1 * MSEC, "block 2");
  sink_send(t, 2100, 2500);
  expect_ack(t, 0, 1 * MSEC, "block 2 extended");
  CHECK(sink_ok(0), "data behind a hole was delivered");

  // Each hole filled moves the ACK to the next one
  sink_send(t, 0, 1000);
  expect_ack(t, 1200, 1 * MSEC, "first hole");
  sink_send(t, 1200, 2000);
  expect_ack(t, 2500, 1 * MSEC, "second hole");
  sink_send(t, 2400, 3100); // Overlaps on both sides
  expect_ack(t, 3200, 1 * MSEC, "third hole");
  CHECK(sink_ok(3200), "sink has %u bytes", sink_len);

  // More separate blocks than are tracked (four): the fifth is not
  // kept, and gets no duplicate ACK that would claim it was
  for(int i = 0; i < 4; i++) {
    sink_send(t, 3300 + i * 200, 3400 + i * 200);
    expect_ack(t, 3200, 1 * MSEC, "one of four blocks");
  }
  sink_send(t, 4100, 4200);
  expect_quiet(t, 5 * MSEC, "fifth block");
  sink_send(t, 3200, 3300);
  expect_ack(t, 3400, 1 * MSEC, "hole before four blocks");
  sink_send(t, 3400, 4100);
  expect_ack(t, 4100, 1 * MSEC, "the rest");
  CHECK(sink_ok(4100), "sink has %u bytes", sink_len);

  // The FIN can wait behind a hole as well
  uint8_t last[100];
  for(int i = 0; i < 100; i++)
    last[i] = pattern(4200 + i);
  sim_tcp_send(t, SIM_TCP_FIN | SIM_TCP_ACK | SIM_TCP_PSH, 4200, 0, 65535,
               last, sizeof(last));
  expect_ack(t, 4100, 1 * MSEC, "last segment behind a hole");
  CHECK(!sink_eof, "FIN behind a hole closed the stream");
  sink_send(t, 4100, 4200);
  // Data and FIN may be acked together or one after the other
  uint32_t acked = 0;
  while(acked != 4301 && sim_tcp_recv(t, &seg, 50 * MSEC))
    acked = seg.ack;
  CHECK(acked == 4301, "acked %u, expected the FIN at 4301", acked);
  CHECK(sink_ok(4300), "sink has %u bytes at the end", sink_len);
  CHECK(sink_eof, "sink did not see the close");
  if(CHECK(sim_tcp_recv(t, &seg, SEC) && (seg.flags & SIM_TCP_FIN),
           "no FIN from Mios"))
    sim_tcp_send(t, SIM_TCP_ACK, 4301, 1, 65535, NULL, 0);
}


static void
scn_ack_behind_hole(void)
{
  hosttest_log("---- receive: ACK on a segment behind a hole ----");
  sim_tcp_t *t = peer_new(PORT_SOURCE);
  uint32_t cmdseq = 0;
  const uint8_t junk[8] = {};

  if(!CHECK(sim_tcp_connect(t, 65535, NULL), "handshake failed"))
    return;

  // The peer acks our data on a segment that we cannot take because
  // one before it was lost. The ACK counts all the same: no
  // retransmission
  source_request(t, &cmdseq, 0, MSS);
  CHECK(collect(t, 0, MSS) == 1, "expected one segment");
  sim_tcp_send(t, SIM_TCP_ACK | SIM_TCP_PSH, cmdseq + 100, MSS, 65535,
               junk, sizeof(junk));
  expect_ack(t, cmdseq, 1 * MSEC, "segment behind a hole");
  expect_quiet(t, SEC, "data was acked");

  peer_close(t, cmdseq, MSS);
}


static void
scn_rtx_with_unsent_tail(void)
{
  hosttest_log("---- retransmission while more data waits unsent ----");
  sim_tcp_t *t = peer_new(0);
  sim_tcp_seg_t seg;

  client(CLIENT_CONNECT, t->port);
  if(!CHECK(sim_tcp_accept(t, 65535, NULL, SEC), "handshake failed"))
    return;
  client_wait();

  // 100 bytes go out and are lost. 50 more are written while they are
  // in flight and wait (Nagle)
  client(CLIENT_WRITE, 100);
  if(CHECK(sim_tcp_recv(t, &seg, SEC), "no data"))
    CHECK(seg.seq == 0 && seg.len == 100, "seq=%u len=%d", seg.seq, seg.len);
  client_wait();
  client(CLIENT_WRITE, 50);
  client_wait();

  // The retransmission is acked, whatever it carried, and the rest
  // follows. All 150 bytes get through and then it is quiet
  uint32_t rcv_nxt = 0;
  const uint64_t deadline = clock_get() + 2 * SEC;
  while(rcv_nxt != 150 && clock_get() < deadline) {
    if(!sim_tcp_recv(t, &seg, SEC))
      break;
    if(seg.len && seg.seq <= rcv_nxt && seg.seq + seg.len > rcv_nxt)
      rcv_nxt = seg.seq + seg.len;
    ack(t, 0, rcv_nxt, 65535);
  }
  CHECK(rcv_nxt == 150, "got %u bytes of 150", rcv_nxt);
  expect_quiet(t, 2 * SEC, "all acked");
  CHECK(t->rx_segments <= 5, "%d segments for 150 bytes", t->rx_segments);

  sim_tcp_send(t, SIM_TCP_RST, 0, 0, 0, NULL, 0);
  client(CLIENT_CLOSE, 0);
  client_wait();
}


static void
scn_close_with_small_window(void)
{
  hosttest_log("---- close with data waiting for the peer's window ----");
  sim_tcp_t *t = peer_new(0);
  sim_tcp_seg_t seg;

  client(CLIENT_CONNECT, t->port);
  if(!CHECK(sim_tcp_accept(t, MSS + 1, NULL, SEC), "handshake failed"))
    return;
  client_wait();

  // 5000 bytes and a close, into a window that has room for one
  // segment and one byte. The FIN must come after all the data
  client(CLIENT_WRITE, 5000);
  client_wait();
  client(CLIENT_CLOSE, 0);
  client_wait();

  // The peer acks only when Mios has filled the window and stopped
  uint32_t rcv_nxt = 0;
  int got_fin = 0;
  for(int i = 0; i < 20 && !got_fin; i++) {
    while(!got_fin && sim_tcp_recv(t, &seg, 10 * MSEC)) {
      if(seg.seq == rcv_nxt) {
        if(!CHECK(data_ok(&seg), "bad data at %u", seg.seq))
          break;
        rcv_nxt += seg.len;
        if(seg.flags & SIM_TCP_FIN)
          got_fin = 1;
      }
    }
    sim_tcp_send(t, SIM_TCP_ACK, 0, rcv_nxt + got_fin, MSS + 1, NULL, 0);
  }
  CHECK(got_fin && rcv_nxt == 5000, "FIN:%d after %u bytes of 5000",
        got_fin, rcv_nxt);
  sim_tcp_send(t, SIM_TCP_FIN | SIM_TCP_ACK, 0, rcv_nxt + 1, MSS + 1, NULL, 0);
  sim_tcp_recv(t, &seg, SEC);
}


static void
scn_backoff_and_timeout(void)
{
  hosttest_log("---- retransmission backoff, reset on timeout ----");
  sim_tcp_t *t = peer_new(PORT_SOURCE);
  sim_tcp_seg_t seg;
  uint32_t cmdseq = 0;

  if(!CHECK(sim_tcp_connect(t, 65535, NULL), "handshake failed"))
    return;

  // The peer goes away. The segment is sent again as a tail loss
  // probe, then retransmitted at doubling intervals up to a ceiling,
  // and after 21 s of silence Mios gives up and says so with a RST
  source_request(t, &cmdseq, 0, MSS);
  CHECK(collect(t, 0, MSS) == 1, "expected one segment");
  uint64_t prev = clock_get();
  static const int expect_ms[] = { 50, 250, 500, 1000, 2000, 4000, 4000, 4000,
                                   4000 };
  for(int i = 0; i < 9; i++) {
    if(!CHECK(sim_tcp_recv(t, &seg, 10 * SEC), "no retransmission %d", i))
      return;
    const int ms = (seg.at - prev) / 1000;
    CHECK(seg.seq == 0 && seg.len == MSS, "retransmission %d: seq=%u len=%d",
          i, seg.seq, seg.len);
    CHECK(ms >= expect_ms[i] - 10 && ms <= expect_ms[i] + 10,
          "retransmission %d after %d ms, expected %d", i, ms, expect_ms[i]);
    prev = seg.at;
  }
  if(CHECK(sim_tcp_recv(t, &seg, 10 * SEC), "no RST"))
    CHECK((seg.flags & SIM_TCP_RST) && seg.seq == MSS, "flags=%x seq=%u",
          seg.flags, seg.seq);
  expect_quiet(t, 15 * SEC, "after the RST");
}


static void
scn_slow_peer(void)
{
  hosttest_log("---- round trip time longer than the initial RTO ----");
  sim_tcp_t *t = peer_new(PORT_SOURCE);
  sim_tcp_seg_t seg;
  uint32_t cmdseq = 0;

  if(!CHECK(sim_tcp_connect(t, 65535, NULL), "handshake failed"))
    return;

  // Every ACK takes 400 ms. The first segments are retransmitted
  // before it comes, then the timeout has adapted
  int rtx[8] = {};
  for(int i = 0; i < 8; i++) {
    source_request(t, &cmdseq, i * MSS, MSS);
    CHECK(collect(t, i * MSS, (i + 1) * MSS) == 1, "expected one segment");
    const uint64_t deadline = clock_get() + 400 * MSEC;
    while(clock_get() < deadline &&
          sim_tcp_recv(t, &seg, deadline - clock_get()))
      rtx[i]++;
    sim_wait(deadline);
    ack(t, cmdseq, (i + 1) * MSS, 65535);
  }
  CHECK(rtx[0] + rtx[1] + rtx[2] <= 3, "retransmissions %d %d %d",
        rtx[0], rtx[1], rtx[2]);
  for(int i = 3; i < 8; i++)
    CHECK(rtx[i] == 0, "round %d: %d spurious retransmissions", i, rtx[i]);

  peer_close(t, cmdseq, 8 * MSS);
}


static void
scn_zero_window(void)
{
  hosttest_log("---- peer's window closed for a long time ----");
  sim_tcp_t *t = peer_new(0);
  sim_tcp_seg_t seg;

  client(CLIENT_CONNECT, t->port);
  if(!CHECK(sim_tcp_accept(t, 65535, NULL, SEC), "handshake failed"))
    return;
  client_wait();
  ack(t, 0, 0, 0); // Window closed
  sim_wait(clock_get() + 10 * MSEC);

  // Data waits. Mios probes with one byte, at growing intervals; the
  // peer answers that the window is still closed. That goes on for
  // longer than the 21 s after which a silent peer is given up on
  client(CLIENT_WRITE, 3000);
  const uint64_t t0 = clock_get();
  int probes = 0;
  while(clock_get() < t0 + 40 * SEC) {
    if(!sim_tcp_recv(t, &seg, 10 * SEC))
      break;
    if(seg.seq == (uint32_t)-1) {
      ack(t, 0, 0, 0); // Keepalive
      continue;
    }
    if(!CHECK(seg.seq == 0 && seg.len == 1 && data_ok(&seg),
              "probe %d: seq=%u len=%d flags=%x", probes, seg.seq, seg.len,
              seg.flags))
      return;
    if(probes == 0) {
      const int ms = (seg.at - t0) / 1000;
      CHECK(ms >= 200 && ms <= 300, "first probe after %d ms", ms);
    }
    probes++;
    ack(t, 0, 0, 0);
  }
  CHECK(probes >= 8 && probes <= 20, "%d probes in 40 s", probes);

  // The window opens: everything comes, at once
  ack(t, 0, 0, 65535);
  uint32_t rcv_nxt = 0;
  const uint64_t t1 = clock_get();
  while(rcv_nxt != 3000 && sim_tcp_recv(t, &seg, SEC)) {
    if(seg.len && seg.seq <= rcv_nxt && seg.seq + seg.len > rcv_nxt) {
      CHECK(data_ok(&seg), "bad data at %u", seg.seq);
      rcv_nxt = seg.seq + seg.len;
    }
    ack(t, 0, rcv_nxt, 65535);
  }
  CHECK(rcv_nxt == 3000, "got %u bytes of 3000", rcv_nxt);
  const int ms = (clock_get() - t1) / 1000;
  CHECK(ms < 10, "data came %d ms after the window opened", ms);

  client_wait();
  sim_tcp_send(t, SIM_TCP_RST, 0, 0, 0, NULL, 0);
  client(CLIENT_CLOSE, 0);
  client_wait();
}


// The SACK blocks for the peer's next ACKs: n pairs of start, end
static void
sack_set(sim_tcp_t *t, int n, ...)
{
  va_list ap;
  va_start(ap, n);
  t->nsack = n;
  for(int i = 0; i < n; i++) {
    t->sack[i][0] = va_arg(ap, uint32_t);
    t->sack[i][1] = va_arg(ap, uint32_t);
  }
  va_end(ap);
}

// Expect an ACK for 'ackno' with exactly these n SACK blocks, in order
static void
expect_sack(sim_tcp_t *t, uint32_t ackno, const char *what, int n, ...)
{
  sim_tcp_seg_t seg;
  if(!CHECK(sim_tcp_recv(t, &seg, 1 * MSEC), "%s: no ACK", what))
    return;
  CHECK(seg.flags == SIM_TCP_ACK && seg.len == 0 && seg.ack == ackno,
        "%s: flags=%x len=%d ack=%u, expected ack=%u", what, seg.flags,
        seg.len, seg.ack, ackno);
  CHECK(seg.nsack == n, "%s: %d SACK blocks, expected %d", what,
        seg.nsack, n);
  va_list ap;
  va_start(ap, n);
  for(int i = 0; i < n && i < seg.nsack; i++) {
    const uint32_t start = va_arg(ap, uint32_t);
    const uint32_t end = va_arg(ap, uint32_t);
    CHECK(seg.sack[i][0] == start && seg.sack[i][1] == end,
          "%s: block %d is %u-%u, expected %u-%u", what, i,
          seg.sack[i][0], seg.sack[i][1], start, end);
  }
  va_end(ap);
}


static void
scn_sack_receive(void)
{
  hosttest_log("---- SACK: what Mios reports ----");
  sim_tcp_t *t = peer_new(PORT_SINK8K);
  sim_tcp_seg_t synack;

  t->sack_permitted = 1;
  if(!CHECK(sim_tcp_connect(t, 65535, &synack), "handshake failed"))
    return;
  CHECK(synack.sack_permitted, "SYN-ACK without SACK-permitted");

  sink_send(t, 0, 1000);
  expect_ack(t, 1000, 30 * MSEC, "in order");

  // Blocks behind a hole: the one the segment went into comes first,
  // then the others
  sink_send(t, 2000, 3000);
  expect_sack(t, 1000, "one block", 1, 2000, 3000);
  sink_send(t, 4000, 5000);
  expect_sack(t, 1000, "two blocks", 2, 4000, 5000, 2000, 3000);
  sink_send(t, 6000, 7000);
  expect_sack(t, 1000, "three blocks", 3, 6000, 7000, 2000, 3000,
              4000, 5000);
  sink_send(t, 2500, 3500);
  expect_sack(t, 1000, "first block extended", 3, 2000, 3500, 4000, 5000,
              6000, 7000);
  sink_send(t, 3500, 4000);
  expect_sack(t, 1000, "two blocks joined", 2, 2000, 5000, 6000, 7000);

  // The hole is filled: what is left is still reported
  sink_send(t, 1000, 2000);
  expect_sack(t, 5000, "first hole filled", 1, 6000, 7000);
  sink_send(t, 5000, 6000);
  expect_sack(t, 7000, "all holes filled", 0);
  CHECK(sink_ok(7000), "sink has %u bytes", sink_len);
  peer_close(t, 7000, 0);

  // Not without the peer's SACK-permitted
  t = peer_new(PORT_SINK8K);
  if(!CHECK(sim_tcp_connect(t, 65535, &synack), "handshake failed"))
    return;
  CHECK(!synack.sack_permitted, "SACK-permitted that was not asked for");
  sink_send(t, 1000, 2000);
  expect_sack(t, 0, "not negotiated", 0);
  sink_send(t, 0, 1000);
  expect_ack(t, 2000, 1 * MSEC, "hole filled");
  peer_close(t, 2000, 0);
}


static void
scn_sack_send(void)
{
  hosttest_log("---- SACK: several losses, recovered in one round trip ----");
  sim_tcp_t *t = peer_new(PORT_SOURCE);
  sim_tcp_seg_t seg;
  uint32_t cmdseq = 0;

  t->sack_permitted = 1;
  if(!CHECK(sim_tcp_connect(t, 65535, NULL), "handshake failed"))
    return;

  // Ten segments; the second, fifth and sixth are lost. The peer acks
  // the others as they arrive
  source_request(t, &cmdseq, 0, 10 * MSS);
  CHECK(collect(t, 0, 10 * MSS) == 10, "expected ten full segments");
  const uint64_t t0 = clock_get();
  ack(t, cmdseq, 1 * MSS, 65535);
  sack_set(t, 1, 2 * MSS, 3 * MSS);
  ack(t, cmdseq, 1 * MSS, 65535);
  sack_set(t, 1, 2 * MSS, 4 * MSS);
  ack(t, cmdseq, 1 * MSS, 65535);
  expect_quiet(t, 10 * MSEC, "two segments behind the hole");

  // The third one says that the first hole is a loss, and shows the
  // second hole: both are retransmitted at once, all of them
  sack_set(t, 2, 6 * MSS, 7 * MSS, 2 * MSS, 4 * MSS);
  ack(t, cmdseq, 1 * MSS, 65535);
  expect_rtx(t, 1 * MSS, 1 * MSEC, "first hole");
  expect_rtx(t, 4 * MSS, 1 * MSEC, "second hole");
  expect_rtx(t, 5 * MSS, 1 * MSEC, "second hole, second segment");

  // The rest of the window arrives. Nothing is sent twice
  for(int i = 8; i <= 10; i++) {
    sack_set(t, 2, 6 * MSS, i * MSS, 2 * MSS, 4 * MSS);
    ack(t, cmdseq, 1 * MSS, 65535);
  }
  expect_quiet(t, 10 * MSEC, "more of the same");

  // The retransmissions arrive one by one
  sack_set(t, 1, 6 * MSS, 10 * MSS);
  ack(t, cmdseq, 4 * MSS, 65535);
  ack(t, cmdseq, 5 * MSS, 65535);
  sack_set(t, 0);
  ack(t, cmdseq, 10 * MSS, 65535);
  const int ms = (clock_get() - t0) / 1000;
  CHECK(ms < 30, "recovery took %d ms", ms);
  expect_quiet(t, 2 * SEC, "all acked");

  // ACKs get lost too. One that reports three segments behind a hole
  // is enough
  source_request(t, &cmdseq, 10 * MSS, 6 * MSS);
  CHECK(collect(t, 10 * MSS, 16 * MSS) == 6, "expected six full segments");
  sack_set(t, 1, 11 * MSS, 14 * MSS);
  ack(t, cmdseq, 10 * MSS, 65535);
  expect_rtx(t, 10 * MSS, 1 * MSEC, "one ACK with three segments");

  // If the retransmission is lost as well, the timeout sends it again.
  // The peer no longer has what it reported: Mios must not rely on it
  // but retransmit as partial ACKs come
  sack_set(t, 0);
  expect_rtx(t, 10 * MSS, 300 * MSEC, "timeout");
  ack(t, cmdseq, 11 * MSS, 65535);
  expect_rtx(t, 11 * MSS, 1 * MSEC, "reneged block");
  ack(t, cmdseq, 16 * MSS, 65535);
  if(sim_tcp_recv(t, &seg, 2 * SEC))
    CHECK(seg.seq == (uint32_t)-1, "unexpected seq=%u len=%d", seg.seq,
          seg.len);

  peer_close(t, cmdseq, 16 * MSS);
}


static void
scn_sack_active_open(void)
{
  hosttest_log("---- SACK: connection opened by Mios ----");
  sim_tcp_t *t = peer_new(0);
  sim_tcp_seg_t syn, seg;

  // Mios offers it; this peer does not take it up
  client(CLIENT_CONNECT, t->port);
  if(!CHECK(sim_tcp_accept(t, 65535, &syn, SEC), "handshake failed"))
    return;
  CHECK(syn.sack_permitted, "SYN without SACK-permitted");
  client_wait();

  static uint8_t data[1000];
  for(size_t i = 0; i < sizeof(data); i++)
    data[i] = pattern(1000 + i);
  sim_tcp_send(t, SIM_TCP_ACK | SIM_TCP_PSH, 1000, 0, 65535, data,
               sizeof(data));
  expect_sack(t, 0, "not negotiated", 0);

  sim_tcp_send(t, SIM_TCP_RST, 0, 0, 0, NULL, 0);
  client(CLIENT_CLOSE, 0);
  client_wait();

  // This one does
  t = peer_new(0);
  t->sack_permitted = 1;
  client(CLIENT_CONNECT, t->port);
  if(!CHECK(sim_tcp_accept(t, 65535, &syn, SEC), "handshake failed"))
    return;
  client_wait();
  sim_tcp_send(t, SIM_TCP_ACK | SIM_TCP_PSH, 1000, 0, 65535, data,
               sizeof(data));
  expect_sack(t, 0, "negotiated", 1, 1000, 2000);

  // And Mios acts on what the peer reports
  client(CLIENT_WRITE, 5 * MSS);
  CHECK(collect(t, 0, 5 * MSS) == 5, "expected five full segments");
  sack_set(t, 1, 1 * MSS, 4 * MSS);
  ack(t, 0, 0, 65535);
  if(CHECK(sim_tcp_recv(t, &seg, 1 * MSEC), "no retransmission"))
    CHECK(seg.seq == 0 && seg.len == MSS, "got seq=%u len=%d", seg.seq,
          seg.len);

  sim_tcp_send(t, SIM_TCP_RST, 0, 5 * MSS, 0, NULL, 0);
  client(CLIENT_CLOSE, 0);
  client_wait();
}


static void
scn_tail_loss(void)
{
  hosttest_log("---- tail loss probe ----");
  sim_tcp_t *t = peer_new(PORT_SOURCE);
  uint32_t cmdseq = 0;

  if(!CHECK(sim_tcp_connect(t, 65535, NULL), "handshake failed"))
    return;

  // Only the last of five segments is lost: nothing arrives after it,
  // so no duplicate ACKs. The probe is that segment
  source_request(t, &cmdseq, 0, 5 * MSS);
  CHECK(collect(t, 0, 5 * MSS) == 5, "expected five full segments");
  uint64_t t0 = clock_get();
  ack(t, cmdseq, 4 * MSS, 65535);
  expect_quiet(t, 40 * MSEC, "before the probe");
  expect_rtx(t, 4 * MSS, 20 * MSEC, "probe");
  ack(t, cmdseq, 5 * MSS, 65535);
  expect_quiet(t, 2 * SEC, "all acked");

  // The last three of ten are lost. The probe is the last one; the
  // peer's answer, one duplicate ACK, shows that more is missing
  source_request(t, &cmdseq, 5 * MSS, 10 * MSS);
  CHECK(collect(t, 5 * MSS, 15 * MSS) == 10, "expected ten full segments");
  t0 = clock_get();
  ack(t, cmdseq, 12 * MSS, 65535);
  expect_rtx(t, 14 * MSS, 60 * MSEC, "probe");
  ack(t, cmdseq, 12 * MSS, 65535);
  expect_rtx(t, 12 * MSS, 1 * MSEC, "first of the tail");
  ack(t, cmdseq, 13 * MSS, 65535);
  expect_rtx(t, 13 * MSS, 1 * MSEC, "second of the tail");
  ack(t, cmdseq, 15 * MSS, 65535);
  int ms = (clock_get() - t0) / 1000;
  CHECK(ms < 70, "recovery took %d ms", ms);
  expect_quiet(t, 2 * SEC, "all acked");

  // One probe only: if it is lost as well the timeout takes over
  source_request(t, &cmdseq, 15 * MSS, 3 * MSS);
  CHECK(collect(t, 15 * MSS, 18 * MSS) == 3, "expected three full segments");
  t0 = clock_get();
  ack(t, cmdseq, 16 * MSS, 65535);
  expect_rtx(t, 17 * MSS, 60 * MSEC, "probe");
  expect_quiet(t, 200 * MSEC, "after the probe");
  expect_rtx(t, 16 * MSS, 100 * MSEC, "timeout");
  ms = (clock_get() - t0) / 1000;
  CHECK(ms >= 290 && ms <= 310, "timeout after %d ms", ms);
  ack(t, cmdseq, 18 * MSS, 65535);
  expect_quiet(t, 2 * SEC, "all acked");
  peer_close(t, cmdseq, 18 * MSS);

  // With SACK the answer to the probe shows the whole hole
  t = peer_new(PORT_SOURCE);
  cmdseq = 0;
  t->sack_permitted = 1;
  if(!CHECK(sim_tcp_connect(t, 65535, NULL), "handshake failed"))
    return;
  source_request(t, &cmdseq, 0, 10 * MSS);
  CHECK(collect(t, 0, 10 * MSS) == 10, "expected ten full segments");
  ack(t, cmdseq, 7 * MSS, 65535);
  expect_rtx(t, 9 * MSS, 60 * MSEC, "probe");
  sack_set(t, 1, 9 * MSS, 10 * MSS);
  ack(t, cmdseq, 7 * MSS, 65535);
  expect_rtx(t, 7 * MSS, 1 * MSEC, "first of the tail");
  expect_rtx(t, 8 * MSS, 1 * MSEC, "second of the tail");
  sack_set(t, 0);
  ack(t, cmdseq, 10 * MSS, 65535);
  expect_quiet(t, 2 * SEC, "all acked");
  peer_close(t, cmdseq, 10 * MSS);
}


static void
script_thread(void *arg)
{
  scn_transfer();
  scn_sink();
  scn_rto();
  scn_fast_retransmit();
  scn_multiple_losses();
  scn_timeout_recovery();
  scn_active_open();
  scn_write_before_established();
  scn_rx_overlap();
  scn_rx_window();
  scn_dup_ack_window();
  scn_out_of_order();
  scn_ack_behind_hole();
  scn_rtx_with_unsent_tail();
  scn_close_with_small_window();
  scn_backoff_and_timeout();
  scn_slow_peer();
  scn_zero_window();
  scn_sack_receive();
  scn_sack_send();
  scn_sack_active_open();
  scn_tail_loss();
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
run_tcp(void)
{
  // A static address and no link-up event, so no DHCP client
  const uint8_t mac[6] = { 0x02, 0xc0, 0x00, 0x00, 0x09, 0x02 };
  tcp_vnet = vnet_create("veth9", mac);
  netif_t *ni = &vnet_eni(tcp_vnet)->eni_ni;
  mios_ip = ip4(10, 9, 0, 2);
  peer_ip = ip4(10, 9, 0, 1);
  ni->ni_ipv4_local_addr = mios_ip;
  ni->ni_ipv4_local_prefixlen = 24;

  sim_thread_create("tcp-script", script_thread, NULL, 0);

  // Serve the script's client requests until it is done
  static uint8_t buf[32768];
  stream_t *sk = NULL;
  const uint64_t deadline = clock_get() + 600 * SEC;
  while(!script_done && clock_get() < deadline) {
    switch(client_op) {
    case CLIENT_CONNECT:
      sk = tcp_create_socket("tcptest-client", 32768, 2048);
      tcp_connect(sk, peer_ip, client_arg);
      break;
    case CLIENT_WRITE:
      for(uint32_t i = 0; i < client_arg; i++)
        buf[i] = pattern(i);
      stream_write(sk, buf, client_arg, 0);
      break;
    case CLIENT_CLOSE:
      stream_close(sk);
      sk = NULL;
      break;
    }
    client_op = 0;
    usleep(1000);
  }
  CHECK(script_done, "script did not finish");
  CHECK(!sim_tcp_bad_cksum, "%d segments from Mios had a bad checksum",
        sim_tcp_bad_cksum);
  return 0; // failures are counted by CHECK()
}

HOSTTEST_SUITE("tcp", run_tcp, 0);
