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
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/param.h>

#include <mios/service.h>
#include <mios/stream.h>
#include <mios/task.h>

#include <net/ether.h>

#include "hosttest.h"
#include "sim.h"
#include "sim_tcp.h"

#define MSEC 1000ull
#define SEC  1000000ull

#define MSS 1460

#define PORT_SOURCE 7000
#define PORT_SINK   7001

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
   connection at a time. */

static uint8_t sink_buf[65536];
static volatile uint32_t sink_len;
static volatile int sink_eof;

static void *
sink_thread(void *arg)
{
  stream_t *s = arg;
  while(1) {
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

  // Nothing is acked: the first segment comes again after the RTO
  if(CHECK(sim_tcp_recv(t, &seg, SEC), "no retransmission")) {
    const int ms = (seg.at - t0) / 1000;
    CHECK(seg.seq == 0 && seg.len == MSS && data_ok(&seg),
          "retransmitted seq=%u len=%d", seg.seq, seg.len);
    CHECK(ms >= 200 && ms <= 300, "retransmitted after %d ms", ms);
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

  // The whole burst of four is lost, so there are no duplicate ACKs.
  // The timeout finds the first; the rest follow on the partial ACKs
  source_request(t, &cmdseq, 0, 4 * MSS);
  CHECK(collect(t, 0, 4 * MSS) == 4, "expected four full segments");
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
script_thread(void *arg)
{
  scn_transfer();
  scn_sink();
  scn_rto();
  scn_fast_retransmit();
  scn_multiple_losses();
  scn_timeout_recovery();
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
pred_done(void *arg)
{
  return script_done;
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

  CHECK(hosttest_wait(pred_done, NULL, 600 * SEC), "script did not finish");
  return 0; // failures are counted by CHECK()
}

HOSTTEST_SUITE("tcp", run_tcp, 0);
