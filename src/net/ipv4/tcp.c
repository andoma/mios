#include "tcp.h"

#include <sys/param.h>
#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <malloc.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <inttypes.h>

#include "net/pbuf.h"
#include "net/ipv4/ipv4.h"
#include "net/net.h"
#include "net/netif.h"

#include "irq.h"

#include <mios/service.h>
#include <mios/eventlog.h>
#include <mios/cli.h>
#include <mios/align.h>

#define TCP_EVENT_CONNECT  0x1
#define TCP_EVENT_CLOSE    0x2
#define TCP_EVENT_EMIT     0x4
#define TCP_EVENT_WND_OPEN 0x8
#define TCP_EVENT_FLUSH    0x10  // Drain TX FIFO immediately, bypassing Nagle
/*
 * Based on these RFCs:
 *
 *  Transmission Control Protocol (TCP)
 *          https://datatracker.ietf.org/doc/html/rfc9293
 *
 *  Computing TCP's Retransmission Timer
 *          https://datatracker.ietf.org/doc/html/rfc6298
 *
 *  TCP Congestion Control (fast retransmit only)
 *          https://datatracker.ietf.org/doc/html/rfc5681
 *
 *  The NewReno Modification to TCP's Fast Recovery Algorithm
 *          https://datatracker.ietf.org/doc/html/rfc6582
 *
 *  Improving TCP's Robustness to Blind In-Window Attacks (sections 3
 *  to 5: RST, SYN and ACK that must match what we expect)
 *          https://datatracker.ietf.org/doc/html/rfc5961
 *
 *  TCP Selective Acknowledgment Options
 *          https://datatracker.ietf.org/doc/html/rfc2018
 *
 *  The RACK-TLP Loss Detection Algorithm for TCP (the tail loss probe
 *  of section 7 only)
 *          https://datatracker.ietf.org/doc/html/rfc8985
 *
 *  A Conservative Loss Recovery Algorithm Based on Selective
 *  Acknowledgment (SACK) for TCP (the retransmission rules, there is
 *  no congestion window for the rest of it to work on)
 *          https://datatracker.ietf.org/doc/html/rfc6675
 *
 */

/*
 * Loss recovery
 *
 * Sent data stays in the TX FIFO until it is acked, so any segment can
 * be rebuilt from snd.una. A loss is detected by three duplicate ACKs
 * (fast retransmit) or, failing that, by the retransmission timer.
 * Either way the first unacked segment is sent again and the
 * connection is "recovering" until everything sent up to that point
 * (tcb_recover) is acked. While recovering, an ACK that advances but
 * stops short of tcb_recover (a partial ACK) points at the next hole,
 * which is retransmitted at once: n losses in one window cost about n
 * round trips rather than n timeouts.
 *
 * Selective acknowledgments
 *
 * If the peer agreed to SACK in the handshake, its ACKs say which
 * blocks it has behind the hole, and we say the same in ours. What the
 * peer has reported is kept in tcb_sack, a few blocks. In recovery all
 * holes below the highest reported block are retransmitted as soon as
 * they are known, each once (tcb_rtx_nxt tells how far that has got),
 * so several losses in a window cost one round trip. A retransmission
 * that is lost in turn waits for the timeout. After a timeout the
 * reports are forgotten, the peer is allowed to drop what it reported.
 * Forgetting a report, also when there are more blocks than fit, only
 * costs a retransmission that was not needed.
 *
 * In our ACKs the blocks go in segments without data only. tcb_ooo is
 * the list already, and the block the last segment went into is
 * reported first as it should be.
 *
 * Tail loss probe
 *
 * Duplicate ACKs only come if something arrives after the loss. When
 * the last segments of a burst are lost nothing does, and that would
 * leave it to the retransmission timeout, which is long (250 ms at
 * least) and backs off. So when data is in flight and no ACK has come
 * for two round trip times (TCP_PTO_MIN at least, to let a delayed ACK
 * through, and more if the round trip time varies), one segment is
 * sent as a probe: new data if there is any
 * and the window has room, otherwise the last segment again. If that
 * was all that was missing, its ACK settles it. If not, the peer
 * answers with a duplicate ACK, and after a probe a single one is
 * enough to begin recovery. One probe per flight, then the timeout
 * takes over as before.
 *
 * Out-of-order data
 *
 * The RX FIFO is indexed by sequence number, so a segment that arrives
 * behind a hole is copied to where it belongs in the free part of the
 * FIFO, and all that is kept about it is where it begins and ends
 * (tcb_ooo, a few blocks). When the hole is filled rcv.nxt moves past
 * everything that was waiting. This takes no memory beyond the FIFO
 * the connection has anyway.
 *
 * It matters for more than saving retransmissions. A peer without
 * SACK takes every duplicate ACK to mean that a segment got through.
 * If we acked segments behind a hole and then threw them away, such a
 * peer (Linux for one) would decide the path reorders packets, wait
 * for more duplicate ACKs than we ever send, and end up recovering by
 * timeout.
 *
 * There is no congestion window. The sender is limited by the peer's
 * window, Nagle and the size of the TX FIFO only, and a loss does not
 * slow it down. That fits what this stack is for, a few connections
 * with small buffers on a local network, but it is a deliberate
 * omission and not an oversight.
 */


// Retransmission timeout. It follows the measured round trip time
// (RFC 6298) between these limits and doubles for every timeout in a
// row. The floor is well above the RFC's clock granularity: it has to
// cover the peer's delayed ACK (up to 200 ms on Linux), and since
// fast retransmit handles most losses the timeout is mainly for the
// last segments of a burst.
#define TCP_RTO_MIN  250 // ms, also the initial value
#define TCP_RTO_MAX 4000 // ms

// The least time without an ACK before a tail loss probe is sent. Above
// the delayed ACK of common peers (40 ms on Linux, 20 ms here), or
// every burst that ends in an odd segment would be probed for nothing
#define TCP_PTO_MIN 50 // ms

// A segment that could not be handed to the network interface (its
// transmit queue is full, or there are no buffers) is tried again
// after this long. Nothing else would: the interface does not tell us
// when it has room, and an ACK may not be on its way to do it
#define TCP_TX_RETRY 1000 // us

#define TCP_EMIT_ACK   0x1 // Send an ACK even if there is no data to send
#define TCP_EMIT_PROBE 0x2 // Send one byte even if the window is closed

#define TCP_KEEPALIVE_INTERVAL 10000 // ms
#define TCP_TIMEOUT_INTERVAL   21000 // ms
#define TCP_TIMEOUT_HANDSHAKE   5000 // ms

// Connections that have got a SYN and wait for the handshake to
// finish. Each holds a tcb with its FIFOs, so there is a limit, and a
// SYN beyond it takes the place of the oldest. A real client sends its
// SYN again; one that never meant to answer does not notice
#define TCP_MAX_HALF_OPEN 8

#define TCP_OOO_BLOCKS 4
#define TCP_SACK_BLOCKS 4

// [start, end) in sequence numbers
typedef struct {
  uint32_t start;
  uint32_t end;
} tcp_block_t;

#define TCP_STATE_CLOSED       0
#define TCP_STATE_LISTEN       1
#define TCP_STATE_SYN_SENT     2
#define TCP_STATE_SYN_RECEIVED 3
#define TCP_STATE_ESTABLISHED  4
#define TCP_STATE_FIN_WAIT1    5
#define TCP_STATE_FIN_WAIT2    6
#define TCP_STATE_CLOSING      7
#define TCP_STATE_TIME_WAIT    8
#define TCP_STATE_CLOSE_WAIT   9
#define TCP_STATE_LAST_ACK     10

LIST_HEAD(tcb_list, tcb);

static struct tcb_list tcbs;
static mutex_t tcbs_mutex = MUTEX_INITIALIZER("tcp");

typedef struct tcb {

  stream_t tcb_stream;

  net_task_t tcb_task;

  LIST_ENTRY(tcb) tcb_link;

  uint8_t tcb_state;
  uint8_t tcb_app_closed;

  // An accepted connection is handed to its service when the handshake
  // is done, not before. Until then this says which one it is for
  const service_t *tcb_svc;

  uint8_t tcb_pending_syn;
  uint8_t tcb_pending_fin;

  uint8_t tcb_fin_received;
  uint8_t rcv_window_closed;

  // Data received behind a hole, see the comment at the top. Sorted,
  // apart and all ahead of rcv.nxt. A FIN can wait there too
  uint8_t tcb_ooo_count;
  uint8_t tcb_ooo_fin;
  uint32_t tcb_ooo_fin_seq;
  uint32_t tcb_ooo_recent; // Where the last segment kept begins
  tcp_block_t tcb_ooo[TCP_OOO_BLOCKS];

  // Selective acknowledgments, see the comment at the top. tcb_sack is
  // what the peer says it has beyond snd.una: sorted and apart
  uint8_t tcb_sack_ok;     // Both sides sent SACK-permitted
  uint8_t tcb_sack_count;
  tcp_block_t tcb_sack[TCP_SACK_BLOCKS];
  uint32_t tcb_rtx_nxt;    // Retransmitted up to here in this recovery

  // ACKs sent for segments we could not take, see tcp_reject_limit()
  uint8_t tcb_reject_count;
  uint32_t tcb_reject_time;

  // Loss recovery, see the comment at the top
  uint8_t tcb_dupacks;
  uint8_t tcb_recovering;
  uint32_t tcb_recover;  // snd.nxt when recovery began

  // Tail loss probe, see the comment at the top
  uint8_t tcb_tlp_armed; // The retransmission timer is set for a probe
  uint8_t tcb_tlp_out;   // A probe has been sent for this flight
  uint8_t tcb_tx_blocked; // A segment could not be sent, try again soon

  // RFC 7323 window scaling state, populated from the WSopt exchange at
  // SYN time. wnd_shift is the peer's announced shift — applied to any
  // wnd field we receive after the handshake. wnd_scale_ok means the
  // peer offered WSopt in its SYN and we should include WSopt in our
  // SYN-ACK. Both default to 0 so non-scaled connections behave exactly
  // as before.
  uint8_t tcb_wnd_scale_ok;
  uint8_t tcb_snd_wnd_shift;

  struct {
    uint32_t wrptr; // Unsent (Enqueued by stream but not yet processed by TCP)
    uint32_t nxt;   // Next to send
    uint32_t una;   // Unacknowledged
    uint32_t wnd;   // Peer's advertised window, already left-shifted by
                    // snd_wnd_shift. Can exceed 64 KB when WS is active.
    uint16_t up;
    uint32_t wl1;
    uint32_t wl2;
  } tcb_snd;
  uint32_t tcb_iss;

  struct {
    uint32_t rdptr;
    uint32_t nxt;
    uint16_t up;
    uint16_t mss;     // Largest segment our interface carries, which is
                      // also what we tell the peer to send at most
    uint16_t adv_wnd; // Window in the last segment we sent
  } tcb_rcv;

  uint32_t tcb_local_addr;
  uint32_t tcb_remote_addr;

  uint16_t tcb_remote_port;
  uint16_t tcb_local_port;

  uint16_t tcb_max_segment_size; // The peer's MSS (536 if it said nothing)

  uint32_t tcb_txfifo_size; // Must be power of 2
  uint32_t tcb_rxfifo_size; // Must be power of 2

  timer_t tcb_rtx_timer;
  timer_t tcb_delayed_ack_timer;
  timer_t tcb_time_wait_timer;

  int tcb_rto;    // in ms
  int tcb_srtt;   // in us, negative until there is a measurement
  int tcb_rttvar; // in us

  // Round trip time measurement: one segment at a time is timed, from
  // when it is sent until snd.una reaches tcb_rtt_seq. Never a
  // retransmitted one (Karn's algorithm)
  uint8_t tcb_rtt_pending;
  uint32_t tcb_rtt_seq;
  uint64_t tcb_rtt_start;

  uint64_t tcb_tx_bytes;
  uint64_t tcb_rx_bytes;
  uint64_t tcb_rtx_bytes;
  uint32_t tcb_fast_rtx;
  uint32_t tcb_tlp_count;

  uint64_t tcb_last_rx;
  uint32_t tcb_timo;

  const char *tcb_name;

  uint32_t tcb_rtx_drop;

  task_waitable_t tcb_rx_waitq;
  task_waitable_t tcb_tx_waitq;

  mutex_t tcb_write_mutex;

  uint8_t tcb_buffer[0]; // TX Fifo + RX Fifo

} tcb_t;


static inline uint8_t *
tcb_txfifo(tcb_t *tcb)
{
  return tcb->tcb_buffer;
}

static uint32_t
tcb_txfifo_used(const tcb_t *tcb)
{
  return tcb->tcb_snd.wrptr - tcb->tcb_snd.una;
}

__attribute__((unused))
static uint32_t
tcb_txfifo_avail(const tcb_t *tcb)
{
  return tcb->tcb_txfifo_size - tcb_txfifo_used(tcb);
}

static inline uint8_t *
tcb_rxfifo(tcb_t *tcb)
{
  return tcb->tcb_buffer + tcb->tcb_txfifo_size;
}

static uint32_t
tcb_rxfifo_used(const tcb_t *tcb)
{
  return tcb->tcb_rcv.nxt - tcb->tcb_rcv.rdptr;
}

static uint32_t
tcb_rxfifo_avail(const tcb_t *tcb)
{
  return tcb->tcb_rxfifo_size - tcb_rxfifo_used(tcb);
}


static void
wtol_memcpy(uint8_t *dst, const uint8_t *fifo,
            uint32_t length, uint32_t offset, size_t size)
{
  const size_t mask = size - 1;
  offset &= mask;

  // Compare without masking the end: a copy of the whole FIFO from a
  // nonzero offset ends where it starts and still wraps
  if(offset + length <= size) {
    memcpy(dst, fifo + offset, length);
  } else {
    memcpy(dst, fifo + offset, size - offset);
    memcpy(dst + size - offset, fifo, length - (size - offset));
  }
}


static void
ltow_memcpy(uint8_t *fifo, const uint8_t *src,
            uint32_t length, uint32_t offset, size_t size)
{
  const size_t mask = size - 1;
  offset &= mask;

  if(offset + length <= size) {
    memcpy(fifo + offset, src, length);
  } else {
    memcpy(fifo + offset, src, size - offset);
    memcpy(fifo, src + size - offset, length - (size - offset));
  }
}



// Add [start, end) to a set of at most 'max' blocks, sorted and apart.
// Blocks it touches are merged into it. 'base' is a sequence number
// that is not after any of them, sequence numbers wrap. Returns 0 if
// the set is full, it is then unchanged
static int
tcp_blocks_insert(tcp_block_t *v, uint8_t *count, int max, uint32_t base,
                  uint32_t start, uint32_t end)
{
  int n = 0;

  for(int i = 0; i < *count; i++) {
    const uint32_t bs = v[i].start;
    const uint32_t be = v[i].end;
    if(be - base < start - base || bs - base > end - base) {
      v[n++] = v[i];
      continue;
    }
    if(bs - base < start - base)
      start = bs;
    if(be - base > end - base)
      end = be;
  }
  *count = n;
  if(n == max)
    return 0; // Nothing was merged either, so nothing has changed

  int pos = n;
  while(pos > 0 && v[pos - 1].start - base > start - base) {
    v[pos] = v[pos - 1];
    pos--;
  }
  v[pos].start = start;
  v[pos].end = end;
  *count = n + 1;
  return 1;
}


#define TCP_PBUF_HEADROOM (16 + sizeof(ipv4_header_t) + sizeof(tcp_hdr_t))

static void tcp_close(tcb_t *tcb, const char *reason);

static const char *tcp_state_to_str(int state);


// The largest segment we send: no more than the peer said it can
// take, nor than our interface carries
static inline uint32_t
tcp_snd_mss(const tcb_t *tcb)
{
  return MIN(tcb->tcb_rcv.mss, tcb->tcb_max_segment_size);
}


// What fits in a packet on the interface the connection uses, with
// IPv4 and TCP headers without options
static void
tcp_set_netif(tcb_t *tcb, const netif_t *ni)
{
  const int mtu = ni->ni_mtu ? ni->ni_mtu : 1500;
  tcb->tcb_rcv.mss =
    MAX(mtu - (int)(sizeof(ipv4_header_t) + sizeof(tcp_hdr_t)), 64);
}


// Initial sequence number: one that cannot be guessed, which is what
// keeps someone who is not on the path from forging segments. That is
// as true as rand() is good; a target with a hardware generator
// should have rand() use it. (rand() gives 31 bits)
static uint32_t
tcp_initial_seq(void)
{
  return ((uint32_t)rand() << 16) ^ (uint32_t)rand();
}


static void
tcp_set_state(tcb_t *tcb, int state, const char *reason)
{
  if(tcb->tcb_state == state)
    return;
#if 0
  evlog(LOG_DEBUG, "%s: %s->%s (%s)",
        tcb->tcb_name,
        tcp_state_to_str(tcb->tcb_state),
        tcp_state_to_str(state),
        reason);
#endif
  tcb->tcb_state = state;
}

static error_t
tcp_output(pbuf_t *pb, uint32_t local_addr, uint32_t remote_addr)
{
  nexthop_t *nh = ipv4_nexthop_resolve(remote_addr);
  if(nh == NULL) {
    pbuf_free(pb);
    return ERR_NO_ROUTE;
  }
  netif_t *ni = nh->nh_netif;

  if(local_addr == 0)
    local_addr = ni->ni_ipv4_local_addr;

  tcp_hdr_t *th = pbuf_data(pb, 0);

#if 0
  if(th->flg == (TCP_F_ACK | TCP_F_PSH)) {
    static int x;
    x++;
    if(x == 10) {
      x = 0;
      printf("drop output\n");
      pbuf_free(pb);
      return;
    }
  }
#endif

  th->up = 0;
  th->cksum = 0;

  if(!(ni->ni_flags & NETIF_F_TX_TCP_CKSUM_OFFLOAD)) {
    th->cksum =
      ipv4_cksum_pbuf(ipv4_cksum_pseudo(local_addr, remote_addr,
                                        IPPROTO_TCP, pb->pb_pktlen),
                      pb, 0, pb->pb_pktlen);
  }

  pb = pbuf_prepend(pb, sizeof(ipv4_header_t), 0, 0);
  if(pb == NULL)
    return ERR_NO_BUFFER;

  ipv4_header_t *ip = pbuf_data(pb, 0);

  ip->ver_ihl = 0x45;
  ip->tos = 0;

  ip->total_length = htons(pb->pb_pktlen);
  ip->id = rand();
  ip->fragment_info = htons(IPV4_F_DF);
  ip->ttl = 255;
  ip->proto = IPPROTO_TCP;
  ip->src_addr = local_addr;
  ip->dst_addr = remote_addr;

  ip->cksum = 0;
  if(!(ni->ni_flags & NETIF_F_TX_IPV4_CKSUM_OFFLOAD)) {
    ip->cksum = ipv4_cksum_pbuf(0, pb, 0, sizeof(ipv4_header_t));
  }

  return nh->nh_netif->ni_output_ipv4(ni, nh, pb);
}


// Append a SACK option with what we hold behind the hole to a segment
// that has no data. Returns the number of bytes added
static size_t
tcp_sack_option(tcb_t *tcb, pbuf_t *pb)
{
  if(!tcb->tcb_sack_ok || !tcb->tcb_ooo_count)
    return 0;

  // As many blocks as there is room for, in the packet buffer too
  const size_t room = PBUF_DATA_SIZE - pb->pb_offset - pb->pb_buflen;
  if(room < 12)
    return 0;
  const int n = MIN(tcb->tcb_ooo_count, (room - 4) / 8);

  // The block that the last segment went into comes first
  int first = 0;
  for(int i = 0; i < tcb->tcb_ooo_count; i++) {
    if(tcb->tcb_ooo_recent - tcb->tcb_ooo[i].start <
       tcb->tcb_ooo[i].end - tcb->tcb_ooo[i].start)
      first = i;
  }

  uint8_t *o = pbuf_append(pb, 4 + 8 * n);
  o[0] = 1; // NOP
  o[1] = 1; // NOP
  o[2] = 5; // SACK
  o[3] = 2 + 8 * n;
  o += 4;
  for(int i = 0; i < n; i++) {
    const tcp_block_t *b = &tcb->tcb_ooo[(first + i) % tcb->tcb_ooo_count];
    const uint32_t be[2] = { htonl(b->start), htonl(b->end) };
    memcpy(o, be, 8);
    o += 8;
  }
  return 4 + 8 * n;
}


static error_t
tcp_output_tcb(tcb_t *tcb, pbuf_t *pb, const char *why)
{
  tcp_hdr_t *th = pbuf_data(pb, 0);

  th->src_port = tcb->tcb_local_port;
  th->dst_port = tcb->tcb_remote_port;
  th->ack = htonl(tcb->tcb_rcv.nxt);

  uint32_t rcv_wnd = tcb_rxfifo_avail(tcb);
  tcb->tcb_rcv.adv_wnd = MIN(rcv_wnd, 65535);
  th->wnd = htons(tcb->tcb_rcv.adv_wnd);

  if(th->flg & TCP_F_SYN) {
    uint8_t *opts = pbuf_append(pb, 4);
    opts[0] = 2;
    opts[1] = 4;
    opts[2] = tcb->tcb_rcv.mss >> 8;
    opts[3] = tcb->tcb_rcv.mss;
    int opt_words = 1;

    // Include RFC 7323 WSopt. For SYN-ACK we only include it if the
    // peer's SYN also had one (tcb_wnd_scale_ok was set by
    // tcp_parse_options). For a pure SYN (active open) we always
    // include it — peer will mirror it if supported. Pad with a NOP to
    // keep the options 4-byte aligned.
    int send_wsopt;
    if(th->flg & TCP_F_ACK)
      send_wsopt = tcb->tcb_wnd_scale_ok;
    else
      send_wsopt = 1;

    if(send_wsopt) {
      uint8_t *ws = pbuf_append(pb, 4);
      ws[0] = 1;   // NOP (4-byte alignment)
      ws[1] = 3;   // kind = Window Scale
      ws[2] = 3;   // length
      ws[3] = 0;   // our shift — 0, since our RX FIFO is small enough
                   // that the 16-bit wnd field covers it without scaling
      opt_words++;
    }

    // SACK-permitted, on the same terms
    if(!(th->flg & TCP_F_ACK) || tcb->tcb_sack_ok) {
      uint8_t *sp = pbuf_append(pb, 4);
      sp[0] = 1;   // NOP
      sp[1] = 1;   // NOP
      sp[2] = 4;   // kind = SACK-permitted
      sp[3] = 2;   // length
      opt_words++;
    }

    th->off = ((sizeof(tcp_hdr_t) >> 2) + opt_words) << 4;
  } else {
    size_t optlen = 0;
    if(pb->pb_pktlen == sizeof(tcp_hdr_t))
      optlen = tcp_sack_option(tcb, pb);
    th = pbuf_data(pb, 0);
    th->off = ((sizeof(tcp_hdr_t) + optlen) >> 2) << 4;
  }

  return tcp_output(pb, tcb->tcb_local_addr, tcb->tcb_remote_addr);
}


// Start timing a round trip if none is being timed
static void
tcp_rtt_start(tcb_t *tcb)
{
  if(tcb->tcb_rtt_pending || tcb->tcb_recovering)
    return;
  tcb->tcb_rtt_pending = 1;
  tcb->tcb_rtt_seq = tcb->tcb_snd.nxt;
  tcb->tcb_rtt_start = clock_get();
}


// RFC 6298 section 2
static void
tcp_rtt_sample(tcb_t *tcb, int rtt)
{
  if(tcb->tcb_srtt < 0) {
    tcb->tcb_srtt = rtt;
    tcb->tcb_rttvar = rtt / 2;
  } else {
    const int err = rtt > tcb->tcb_srtt ? rtt - tcb->tcb_srtt :
      tcb->tcb_srtt - rtt;
    tcb->tcb_rttvar += (err - tcb->tcb_rttvar) / 4;
    tcb->tcb_srtt += (rtt - tcb->tcb_srtt) / 8;
  }
  const int rto = (tcb->tcb_srtt + 4 * tcb->tcb_rttvar) / 1000;
  tcb->tcb_rto = MAX(TCP_RTO_MIN, MIN(TCP_RTO_MAX, rto));
}


// Send a segment that begins at seq, with as much data as there is and
// fits. A retransmission (seq before snd.nxt) carries at most 'limit'
// bytes
static error_t
tcp_emit_segment(tcb_t *tcb, pbuf_t *pb, uint32_t seq, int flags,
                 uint32_t limit, const char *why)
{
  if(pb == NULL) {
    pb = pbuf_make(TCP_PBUF_HEADROOM, 0);
    if(pb == NULL) {
      return ERR_NO_BUFFER;
    }
  } else {
    // Recycle packet
    pbuf_reset(pb, TCP_PBUF_HEADROOM, 0);
  }

  pb = pbuf_prepend(pb, sizeof(tcp_hdr_t), 0, 0);
  if(pb == NULL) {
    return ERR_NO_BUFFER;
  }

  timer_disarm(&tcb->tcb_delayed_ack_timer);

  tcp_hdr_t *th = pbuf_data(pb, 0);

  // Everything from seq on, a pending FIN included: it has the last
  // sequence number in the FIFO
  const uint32_t fifo_tail = tcb->tcb_snd.wrptr - seq;
  uint32_t bytes_in_fifo = fifo_tail;

  if(seq == tcb->tcb_snd.nxt) {
    // Fresh data: honor the receiver's advertised window
    uint32_t in_flight = tcb->tcb_snd.nxt - tcb->tcb_snd.una;
    uint32_t wnd_room = tcb->tcb_snd.wnd > in_flight
                          ? tcb->tcb_snd.wnd - in_flight : 0;
    if((flags & TCP_EMIT_PROBE) && !wnd_room && !in_flight)
      wnd_room = 1;
    bytes_in_fifo = MIN(bytes_in_fifo, wnd_room);
  } else {
    // A retransmission: only what has been sent before. That was
    // inside the window then, and anything written since is not ours
    // to send from here, snd.nxt would not account for it
    bytes_in_fifo = MIN(bytes_in_fifo, tcb->tcb_snd.nxt - seq);
    bytes_in_fifo = MIN(bytes_in_fifo, limit);
  }

  // The FIN goes out only when all that is before it does
  const int fin = tcb->tcb_pending_fin && bytes_in_fifo == fifo_tail;

  if(tcb->tcb_pending_syn) {
    // If we have a pending SYN it's the only thing we may send
    th->flg = tcb->tcb_pending_syn;
    th->seq = htonl(tcb->tcb_iss);
    return tcp_output_tcb(tcb, pb, "pending-syn");

  } else if(!bytes_in_fifo) {
    // Nothing in FIFO to send, but send an ACK if asked to
    if(flags & TCP_EMIT_ACK) {
      th->flg = TCP_F_ACK;
      th->seq = htonl(seq);
      return tcp_output_tcb(tcb, pb, "empty_ack");
    } else {
      pbuf_free(pb);
    }
    return 0;

  } else if(fin) {

    if(bytes_in_fifo == 1) {
      // We only have the last FIN left
      th->flg = tcb->tcb_pending_fin;
      th->seq = htonl(seq);

      if(tcb->tcb_snd.nxt != tcb->tcb_snd.wrptr)
        tcb->tcb_snd.nxt = tcb->tcb_snd.wrptr;

      return tcp_output_tcb(tcb, pb, "fin");
    } else {
      bytes_in_fifo--;
    }

  }

  uint32_t seq0 = seq;

  pbuf_t *p = pb;

  th->flg = TCP_F_PSH | TCP_F_ACK;
  th->seq = htonl(seq);
  int total = 0;

  size_t max_pkt_size = tcp_snd_mss(tcb) + sizeof(tcp_hdr_t);

  while(bytes_in_fifo && pb->pb_pktlen < max_pkt_size) {
    if(p->pb_offset + p->pb_buflen == PBUF_DATA_SIZE) {
      pbuf_t *n = pbuf_make(0, 0);

      if(n == NULL)
        break;

      p->pb_flags &= ~PBUF_EOP;
      n->pb_flags = PBUF_EOP;
      p->pb_next = n;
      p = n;
    }
    size_t avail_in_pbuf = PBUF_DATA_SIZE - p->pb_offset - p->pb_buflen;
    size_t to_copy = MIN(bytes_in_fifo, avail_in_pbuf);
    to_copy = MIN(max_pkt_size - pb->pb_pktlen, to_copy);

    wtol_memcpy(p->pb_data + p->pb_offset + p->pb_buflen,
                tcb_txfifo(tcb), to_copy, seq, tcb->tcb_txfifo_size);

    seq += to_copy;
    bytes_in_fifo -= to_copy;
    p->pb_buflen += to_copy;
    pb->pb_pktlen += to_copy;
    total += to_copy;
  }

  error_t err = tcp_output_tcb(tcb, pb, "data");
  if(err)
    return err;

  // We sent from our head of queue, increase
  if(tcb->tcb_snd.nxt == seq0) {
    tcb->tcb_snd.nxt += total;
    tcb->tcb_tx_bytes += total;
    tcp_rtt_start(tcb);
  } else {
    tcb->tcb_rtx_bytes += total;
  }
  return 0;
}


static error_t
tcp_emit_limit(tcb_t *tcb, pbuf_t *pb, uint32_t seq, int flags,
               uint32_t limit, const char *why)
{
  const error_t err = tcp_emit_segment(tcb, pb, seq, flags, limit, why);
  if(err && !tcb->tcb_pending_syn &&
     tcb->tcb_snd.una != tcb->tcb_snd.wrptr) {
    // Data is waiting and this did not get out
    tcb->tcb_tx_blocked = 1;
    net_timer_arm(&tcb->tcb_rtx_timer, clock_get() + TCP_TX_RETRY);
  }
  return err;
}


static error_t
tcp_emit(tcb_t *tcb, pbuf_t *pb, uint32_t seq, int flags, const char *why)
{
  return tcp_emit_limit(tcb, pb, seq, flags, UINT32_MAX, why);
}


static void
tcp_send_syn(tcb_t *tcb, uint8_t flags, pbuf_t *pb)
{
  if(tcb->tcb_snd.una == tcb->tcb_snd.nxt) {
    // Nothing currently outstanding, arm rtx timer
    net_timer_arm(&tcb->tcb_rtx_timer, clock_get() + tcb->tcb_rto * 1000);
  }

  tcb->tcb_pending_syn = flags;
  tcb->tcb_snd.nxt++;
  tcp_rtt_start(tcb);
  tcp_emit(tcb, pb, tcb->tcb_snd.nxt, 0, "SYN");
}


static void
tcp_send_fin(tcb_t *tcb, uint8_t flags)
{
  if(tcb->tcb_snd.una == tcb->tcb_snd.nxt) {
    // Nothing currently outstanding, arm rtx timer
    net_timer_arm(&tcb->tcb_rtx_timer, clock_get() + tcb->tcb_rto * 1000);
  }

  tcb->tcb_pending_fin = flags;
  tcb->tcb_snd.wrptr++;
  tcp_emit(tcb, NULL, tcb->tcb_snd.nxt, 0, "FIN");
}


static void
arm_rtx(tcb_t *tcb, uint64_t now)
{
  int rtx_duration_ms;

  tcb->tcb_tlp_armed = 0;

  if(tcb->tcb_tx_blocked) {
    // Before anything else: what could not be sent
    net_timer_arm(&tcb->tcb_rtx_timer, now + TCP_TX_RETRY);
    return;
  }

  // Something in flight, or something waiting for the peer's window
  if(tcb->tcb_snd.una != tcb->tcb_snd.wrptr) {
    rtx_duration_ms = tcb->tcb_rto;

    // A tail loss probe first, if it would come sooner. Not when the
    // peer's window is closed: what is in flight then is a zero window
    // probe, with a pace of its own
    if(tcb->tcb_snd.una != tcb->tcb_snd.nxt && !tcb->tcb_recovering &&
       !tcb->tcb_tlp_out && !tcb->tcb_pending_syn && tcb->tcb_srtt >= 0 &&
       tcb->tcb_snd.wnd) {
      // Two round trips, and no sooner than a timeout without its
      // floor would be: a probe that was not needed cannot be told
      // from one that was, so it should be rare
      const int pto = MAX(MAX(2 * tcb->tcb_srtt,
                              tcb->tcb_srtt + 4 * tcb->tcb_rttvar) / 1000,
                          TCP_PTO_MIN);
      if(pto < rtx_duration_ms) {
        rtx_duration_ms = pto;
        tcb->tcb_tlp_armed = 1;
      }
    }
  } else {
    rtx_duration_ms = TCP_KEEPALIVE_INTERVAL;
  }
  net_timer_arm(&tcb->tcb_rtx_timer, now + rtx_duration_ms * 1000);
}


// No ACK for a while with data in flight: send one segment to find out
// if the end of it was lost
static void
tcp_tail_loss_probe(tcb_t *tcb)
{
  const uint32_t nxt = tcb->tcb_snd.nxt;
  const uint32_t in_flight = nxt - tcb->tcb_snd.una;

  tcb->tcb_tlp_out = 1;
  tcb->tcb_tlp_count++;

  // New data, if the window has room for it
  if(tcb->tcb_snd.wrptr != nxt && tcb->tcb_snd.wnd > in_flight) {
    tcp_emit(tcb, NULL, nxt, 0, "tlp");
    if(tcb->tcb_snd.nxt != nxt)
      return;
  }

  // The last segment again. A round trip measurement in progress is
  // left to finish: if this probe was not needed it is because the
  // path is slower than we think, and then the measurement is what
  // tells us. (If the segment was in fact lost the result is too long
  // by the time we waited, which errs on the safe side.)
  const uint32_t mss = tcp_snd_mss(tcb);
  tcp_emit(tcb, NULL, nxt - MIN(mss, in_flight), 0, "tlp-rtx");
}


// snd.una is now 'una': forget what the peer reported below it
static void
tcp_sack_prune(tcb_t *tcb, uint32_t una)
{
  int n = 0;
  for(int i = 0; i < tcb->tcb_sack_count; i++) {
    tcp_block_t b = tcb->tcb_sack[i];
    if((int32_t)(b.end - una) <= 0)
      continue;
    if((int32_t)(b.start - una) < 0)
      b.start = una;
    tcb->tcb_sack[n++] = b;
  }
  tcb->tcb_sack_count = n;
}


static uint32_t
tcp_sack_bytes(const tcb_t *tcb)
{
  uint32_t r = 0;
  for(int i = 0; i < tcb->tcb_sack_count; i++)
    r += tcb->tcb_sack[i].end - tcb->tcb_sack[i].start;
  return r;
}


// Take the SACK blocks from the options of a segment that acks 'ack'.
// Returns nonzero if they told us something new
static int
tcp_sack_input(tcb_t *tcb, const uint8_t *opt, size_t len, uint32_t ack)
{
  tcp_sack_prune(tcb, ack);
  const uint32_t before = tcp_sack_bytes(tcb);
  const uint32_t sent = tcb->tcb_snd.nxt - ack;

  while(len > 0 && opt[0] != 0) {
    if(opt[0] == 1) {
      opt++;
      len--;
      continue;
    }
    if(len < 2 || opt[1] < 2 || opt[1] > len)
      break;

    if(opt[0] == 5) {
      for(int i = 2; i + 8 <= opt[1]; i += 8) {
        uint32_t be[2];
        memcpy(be, opt + i, 8);
        const uint32_t start = ntohl(be[0]);
        const uint32_t end = ntohl(be[1]);

        // Beyond the ACK and within what we have sent. That leaves out
        // reports of duplicates (RFC 2883), which lie below the ACK
        if(end - ack > sent || start - ack >= end - ack || start == ack)
          continue;

        if(tcp_blocks_insert(tcb->tcb_sack, &tcb->tcb_sack_count,
                             TCP_SACK_BLOCKS, ack, start, end))
          continue;

        // No room. The holes are filled from the lowest one up, so the
        // highest block is the one we can best do without
        tcp_block_t *last = &tcb->tcb_sack[TCP_SACK_BLOCKS - 1];
        if(start - ack < last->start - ack) {
          tcb->tcb_sack_count--;
          tcp_blocks_insert(tcb->tcb_sack, &tcb->tcb_sack_count,
                            TCP_SACK_BLOCKS, ack, start, end);
        }
      }
    }
    len -= opt[1];
    opt += opt[1];
  }
  return tcp_sack_bytes(tcb) != before;
}


// Retransmit [seq, end), which may be several segments. Returns how
// far it got
static uint32_t
tcp_rtx_range(tcb_t *tcb, uint32_t seq, uint32_t end)
{
  const uint32_t mss = tcp_snd_mss(tcb);
  while(seq != end) {
    const uint32_t len = MIN(end - seq, mss);
    if(tcp_emit_limit(tcb, NULL, seq, 0, len, "rtx"))
      break;
    seq += len;
  }
  return seq;
}


// In recovery: retransmit what we know is missing and have not
// retransmitted yet. With SACK that is every hole below the highest
// block the peer has reported. If 'una_lost' the segment at snd.una is
// known to be missing as well (recovery begins, or a partial ACK)
static void
tcp_recovery_rtx(tcb_t *tcb, int una_lost)
{
  const uint32_t una = tcb->tcb_snd.una;
  uint32_t seq = una;

  if(tcb->tcb_sack_ok && (int32_t)(tcb->tcb_rtx_nxt - una) > 0)
    seq = tcb->tcb_rtx_nxt;

  for(int i = 0; i < tcb->tcb_sack_count; i++) {
    const tcp_block_t *b = &tcb->tcb_sack[i];
    if((int32_t)(b->start - seq) > 0) {
      seq = tcp_rtx_range(tcb, seq, b->start);
      if(seq != b->start)
        break; // Out of buffers, the rest another time
    }
    if((int32_t)(b->end - seq) > 0)
      seq = b->end;
  }

  if(seq == una && una_lost) {
    // Nothing reported beyond it: one segment
    const uint32_t mss = tcp_snd_mss(tcb);
    seq = tcp_rtx_range(tcb, una, una + MIN(mss, tcb->tcb_snd.nxt - una));
  }
  tcb->tcb_rtx_nxt = seq;
}


static void
tcp_rtx_cb(void *opaque, uint64_t now)
{
  tcb_t *tcb = opaque;

  if(now > tcb->tcb_last_rx + tcb->tcb_timo) {
    // Tell the peer, in case it is still there: otherwise it keeps its
    // end of the connection until it next sends something
    if(tcb->tcb_state != TCP_STATE_SYN_SENT) {
      pbuf_t *pb = pbuf_make(TCP_PBUF_HEADROOM, 0);
      if(pb != NULL)
        pb = pbuf_prepend(pb, sizeof(tcp_hdr_t), 0, 0);
      if(pb != NULL) {
        tcp_hdr_t *th = pbuf_data(pb, 0);
        th->flg = TCP_F_RST | TCP_F_ACK;
        th->seq = htonl(tcb->tcb_snd.nxt);
        tcp_output_tcb(tcb, pb, "timeout");
      }
    }
    tcp_close(tcb, "TCP Timeout");
    return;
  }

  if(tcb->tcb_tx_blocked) {
    // Another attempt at what the network interface had no room for.
    // The timeout proper counts from here
    tcb->tcb_tx_blocked = 0;
    if(tcb->tcb_recovering)
      tcp_recovery_rtx(tcb, (int32_t)(tcb->tcb_rtx_nxt -
                                      tcb->tcb_snd.una) <= 0);
    net_task_raise(&tcb->tcb_task, TCP_EVENT_EMIT);
    arm_rtx(tcb, now);
    return;
  }

  if(tcb->tcb_tlp_armed && tcb->tcb_snd.una != tcb->tcb_snd.nxt) {
    tcp_tail_loss_probe(tcb);
    arm_rtx(tcb, now); // The timeout proper, from here
    return;
  }

  pbuf_t *pb;
  if(tcb->tcb_snd.una == tcb->tcb_snd.wrptr) {
    // Send keep-alive if ESTABLISHED

    pb = NULL;
    if(tcb->tcb_state == TCP_STATE_ESTABLISHED) {

      pb = pbuf_make(TCP_PBUF_HEADROOM, 0);
      if(pb != NULL) {
        pb = pbuf_prepend(pb, sizeof(tcp_hdr_t), 0, 0);
        if(pb != NULL) {
          tcp_hdr_t *th = pbuf_data(pb, 0);
          th->flg = TCP_F_ACK | TCP_F_PSH;
          th->seq = htonl(tcb->tcb_snd.una - 1);
        }
      }
    }

    if(pb)
      tcp_output_tcb(tcb, pb, "ka");

  } else if(tcb->tcb_snd.una == tcb->tcb_snd.nxt) {
    // Data is waiting but nothing is in flight: the peer's window is
    // closed (or an earlier attempt to send failed). Send what the
    // window allows now, and if that is nothing, one byte as a zero
    // window probe. The byte is then in flight and retransmitted like
    // any other, with backoff; the peer's answers show its window and
    // that it is alive.
    tcp_emit(tcb, NULL, tcb->tcb_snd.nxt, TCP_EMIT_PROBE, "probe");
  } else {
    tcb->tcb_dupacks = 0;
    tcb->tcb_recovering = 1;
    tcb->tcb_recover = tcb->tcb_snd.nxt;
    tcb->tcb_rtt_pending = 0;
    tcb->tcb_rto = MIN(tcb->tcb_rto * 2, TCP_RTO_MAX);
    // The peer may have dropped what it reported (RFC 2018 section 8)
    tcb->tcb_sack_count = 0;
    tcb->tcb_rtx_nxt = tcb->tcb_snd.una;
    tcp_recovery_rtx(tcb, 1);
  }

  arm_rtx(tcb, now);
}


// An ACK arrived that says a segment got through which was not the one
// the peer is waiting for: a duplicate ACK (RFC 5681 section 2), or
// one that reports something new by SACK
static void
tcp_dup_ack(tcb_t *tcb, int dup)
{
  if(tcb->tcb_recovering) {
    if(tcb->tcb_sack_ok)
      tcp_recovery_rtx(tcb, 0);
    return;
  }

  if(dup && tcb->tcb_dupacks < 255)
    tcb->tcb_dupacks++;

  // Lost, if three segments have arrived after it. Counted in ACKs or,
  // as ACKs get lost too, in what the peer says it has (RFC 6675).
  // After a tail loss probe there is nothing more to wait for: the
  // peer has the probe and still misses something
  if(tcb->tcb_dupacks < 3 && !(dup && tcb->tcb_tlp_out)) {
    const uint32_t mss = tcp_snd_mss(tcb);
    if(tcp_sack_bytes(tcb) <= 2 * mss)
      return;
  }

  tcb->tcb_dupacks = 0;
  tcb->tcb_recovering = 1;
  tcb->tcb_recover = tcb->tcb_snd.nxt;
  tcb->tcb_rtt_pending = 0;
  tcb->tcb_fast_rtx++;
  tcb->tcb_rtx_nxt = tcb->tcb_snd.una;
  tcp_recovery_rtx(tcb, 1);
  arm_rtx(tcb, clock_get());
}


static void
tcp_ack(tcb_t *tcb, uint32_t count)
{
  if(count == 0)
    return;

  tcb->tcb_snd.una += count;
  tcb->tcb_pending_syn = 0;
  tcb->tcb_tlp_out = 0;
  tcp_sack_prune(tcb, tcb->tcb_snd.una);
  task_wakeup(&tcb->tcb_tx_waitq, 1);

  // A valid measurement also ends any backoff. Without one the
  // backed-off timeout stays, for new segments too (RFC 6298 5.7)
  if(tcb->tcb_rtt_pending &&
     (int32_t)(tcb->tcb_snd.una - tcb->tcb_rtt_seq) >= 0) {
    tcb->tcb_rtt_pending = 0;
    tcp_rtt_sample(tcb, tcb->tcb_last_rx - tcb->tcb_rtt_start);
  }

  arm_rtx(tcb, tcb->tcb_last_rx);

  tcb->tcb_dupacks = 0;
  if(tcb->tcb_recovering) {
    if((int32_t)(tcb->tcb_snd.una - tcb->tcb_recover) >= 0) {
      tcb->tcb_recovering = 0;
    } else {
      // Partial ACK: what is now first in line was lost as well
      tcp_recovery_rtx(tcb, 1);
    }
  }

  // An ACK may have cleared the in-flight condition that caused the
  // emit loop to hold a partial segment under Nagle. Re-trigger emit
  // so the tail drains as soon as una catches up to nxt.
  if(tcb->tcb_snd.wrptr != tcb->tcb_snd.nxt)
    net_task_raise(&tcb->tcb_task, TCP_EVENT_EMIT);
}


// The handshake is done. Returns an error if the connection could not
// be handed to its service, it is then to be reset
static error_t
tcp_established(tcb_t *tcb, const char *reason)
{
  tcb->tcb_timo = TCP_TIMEOUT_INTERVAL * 1000;
  tcp_set_state(tcb, TCP_STATE_ESTABLISHED, reason);

  if(tcb->tcb_svc != NULL) {
    // An accepted connection: now there is someone at the other end,
    // so now the service gets its stream (and may start a thread)
    const service_t *svc = tcb->tcb_svc;
    tcb->tcb_svc = NULL;
    tcb->tcb_app_closed = 0;
    const error_t err = svc->open_stream(&tcb->tcb_stream);
    if(err) {
      tcb->tcb_app_closed = 1;
      return err;
    }
    return 0;
  }

  // If the application closed the stream in the meantime, this is
  // when its data and the FIN can go out
  if(tcb->tcb_app_closed)
    net_task_raise(&tcb->tcb_task, TCP_EVENT_FLUSH | TCP_EVENT_CLOSE);
  return 0;
}


static void
tcp_destroy(tcb_t *tcb)
{
  if(tcb->tcb_state != TCP_STATE_CLOSED)
    return;

  if(!tcb->tcb_app_closed)
    return;

  free(tcb);
}


static tcb_t *
tcb_find(uint32_t remote_addr, uint16_t remote_port, uint16_t local_port)
{
  tcb_t *tcb;
  LIST_FOREACH(tcb, &tcbs, tcb_link) {
    if(tcb->tcb_remote_addr == remote_addr &&
       tcb->tcb_remote_port == remote_port &&
       tcb->tcb_local_port == local_port) {
      return tcb;
    }
  }
  return NULL;
}

static void
tcp_do_connect(tcb_t *tcb)
{
  while(1) {
    uint16_t local_port = rand();
    if(local_port < 1024)
      continue;
    local_port = htons(local_port);
    if(tcb_find(tcb->tcb_remote_addr, tcb->tcb_remote_port, local_port))
      continue;
    tcb->tcb_local_port = local_port;
    break;
  }

  // Set last_rx to now, otherwise we get an almost instant TCP timeout
  // in tcp_rtx_cb()
  tcb->tcb_last_rx = clock_get();

  // The MSS for our SYN and our address, from the interface it
  // leaves by
  nexthop_t *nh = ipv4_nexthop_resolve(tcb->tcb_remote_addr);
  if(nh != NULL) {
    tcp_set_netif(tcb, nh->nh_netif);
    tcb->tcb_local_addr = nh->nh_netif->ni_ipv4_local_addr;
  }

  tcp_send_syn(tcb, TCP_F_SYN, NULL);

  mutex_lock(&tcbs_mutex);
  LIST_INSERT_HEAD(&tcbs, tcb, tcb_link);
  mutex_unlock(&tcbs_mutex);
}


static void
tcp_task_cb(net_task_t *nt, uint32_t signals)
{
  tcb_t *tcb = (void *)nt - offsetof(tcb_t, tcb_task);

  if(signals & TCP_EVENT_CONNECT) {
    tcp_do_connect(tcb);
  }

  if(signals & TCP_EVENT_WND_OPEN) {
    if(tcb->tcb_state == TCP_STATE_ESTABLISHED) {
      tcp_emit(tcb, NULL, tcb->tcb_snd.nxt, 1, "wnd-open");
    }
  }

  if(signals & (TCP_EVENT_EMIT | TCP_EVENT_FLUSH)) {
    const int force = !!(signals & TCP_EVENT_FLUSH);
    switch(tcb->tcb_state) {
    case TCP_STATE_ESTABLISHED:
    case TCP_STATE_FIN_WAIT1:
    case TCP_STATE_FIN_WAIT2:
    case TCP_STATE_CLOSE_WAIT:
    case TCP_STATE_CLOSING:
    case TCP_STATE_LAST_ACK:
    {
      // (Nothing outstanding: the retransmission timer starts here)
      const int idle = tcb->tcb_snd.una == tcb->tcb_snd.nxt;

      while(tcb->tcb_snd.wrptr != tcb->tcb_snd.nxt) {
        uint32_t unsent = tcb->tcb_snd.wrptr - tcb->tcb_snd.nxt;
        uint32_t in_flight = tcb->tcb_snd.nxt - tcb->tcb_snd.una;
        uint32_t wnd_room = tcb->tcb_snd.wnd > in_flight
                              ? tcb->tcb_snd.wnd - in_flight : 0;

        // Send window exhausted: stop. When the peer ACKs, tcp_ack
        // re-raises TCP_EVENT_EMIT and we'll pick up here. If its
        // window stays closed the retransmission timer probes it
        if(wnd_room == 0)
          break;

        // Nagle: hold sub-MSS tails while there is still unACKed data
        // in flight. A FLUSH event (explicit stream flush / close path)
        // bypasses this and drains everything.
        if(!force &&
           unsent < tcp_snd_mss(tcb) &&
           tcb->tcb_snd.una != tcb->tcb_snd.nxt)
          break;

        const uint32_t nxt = tcb->tcb_snd.nxt;
        if(tcp_emit(tcb, NULL, nxt, 0, "event"))
          break;
        if(tcb->tcb_snd.nxt == nxt)
          break; // Nothing more could be sent
      }
      if(idle)
        arm_rtx(tcb, clock_get());
      break;
    }

    default:
      break;
    }
  }

  if(signals & TCP_EVENT_CLOSE) {
    tcb->tcb_app_closed = 1;

    // (Closed during the handshake: the FIN is sent when the
    // connection is established, see tcp_established())
    switch(tcb->tcb_state) {
    case TCP_STATE_ESTABLISHED:
      tcp_set_state(tcb, TCP_STATE_FIN_WAIT1, "stream_close");
      tcp_send_fin(tcb, TCP_F_FIN | TCP_F_ACK);
      break;
    case TCP_STATE_CLOSE_WAIT:
      tcp_set_state(tcb, TCP_STATE_LAST_ACK, "stream_close");
      tcp_send_fin(tcb, TCP_F_FIN | TCP_F_ACK);
      break;
    }

    tcp_destroy(tcb);
  }
}



static pbuf_t *
tcp_reply(struct netif *ni, struct pbuf *pb, uint32_t remote_addr,
          uint32_t seq, uint32_t ack, uint8_t flag, uint32_t rcv_wnd)
{
  tcp_hdr_t *th = pbuf_data(pb, 0);

  const uint16_t src_port = th->src_port;
  const uint16_t dst_port = th->dst_port;

  pbuf_reset(pb, pb->pb_offset, sizeof(tcp_hdr_t));

  th->src_port = dst_port;
  th->dst_port = src_port;
  th->seq = htonl(seq);
  th->ack = htonl(ack);
  th->flg = flag;
  th->wnd = htons(MIN(rcv_wnd, 65535));
  th->off = (sizeof(tcp_hdr_t) >> 2) << 4;

  tcp_output(pb, ni->ni_ipv4_local_addr, remote_addr);
  return NULL;
}

// Answer a segment that was put aside behind a hole, reusing its
// packet: an ACK for rcv.nxt that says what we have beyond it
static pbuf_t *
tcp_reply_dup_ack(tcb_t *tcb, struct pbuf *pb, uint32_t rcv_wnd)
{
  pbuf_reset(pb, pb->pb_offset, sizeof(tcp_hdr_t));

  tcp_hdr_t *th = pbuf_data(pb, 0);
  th->src_port = tcb->tcb_local_port;
  th->dst_port = tcb->tcb_remote_port;
  th->seq = htonl(tcb->tcb_snd.nxt);
  th->ack = htonl(tcb->tcb_rcv.nxt);
  th->flg = TCP_F_ACK;
  th->wnd = htons(MIN(rcv_wnd, 65535));

  const size_t optlen = tcp_sack_option(tcb, pb);
  th = pbuf_data(pb, 0);
  th->off = ((sizeof(tcp_hdr_t) + optlen) >> 2) << 4;

  tcp_output(pb, tcb->tcb_local_addr, tcb->tcb_remote_addr);
  return NULL;
}


struct pbuf *
tcp_reject(struct netif *ni, struct pbuf *pb, uint32_t remote_addr,
           uint16_t port, uint32_t ack, const char *reason)
{

  static int64_t last_log;
  int64_t now = clock_get();
  if(now > last_log + 250000) {
    last_log = now;
    evlog(LOG_NOTICE, "Connection from %Id to port %d rejected -- %s",
          remote_addr, port, reason);
  }
  return tcp_reply(ni, pb, remote_addr, 0, ack, TCP_F_ACK | TCP_F_RST, 0);
}


// rcv.nxt has moved: take along what was waiting behind the hole
static void
tcp_ooo_advance(tcb_t *tcb)
{
  while(tcb->tcb_ooo_count) {
    if((int32_t)(tcb->tcb_ooo[0].start - tcb->tcb_rcv.nxt) > 0)
      break;
    if((int32_t)(tcb->tcb_ooo[0].end - tcb->tcb_rcv.nxt) > 0) {
      tcb->tcb_rx_bytes += tcb->tcb_ooo[0].end - tcb->tcb_rcv.nxt;
      tcb->tcb_rcv.nxt = tcb->tcb_ooo[0].end;
    }
    tcb->tcb_ooo_count--;
    memmove(&tcb->tcb_ooo[0], &tcb->tcb_ooo[1],
            tcb->tcb_ooo_count * sizeof(tcb->tcb_ooo[0]));
  }
}


// Copy 'len' bytes of payload, found 'skip' bytes into the packet, to
// the RX FIFO at sequence number 'seq'
static void
tcp_rxfifo_store(tcb_t *tcb, const pbuf_t *pb, size_t skip, uint32_t seq,
                 uint32_t len)
{
  for(; pb != NULL && len; pb = pb->pb_next) {
    if(skip >= pb->pb_buflen) {
      skip -= pb->pb_buflen;
      continue;
    }
    const uint32_t chunk = MIN(pb->pb_buflen - skip, len);
    ltow_memcpy(tcb_rxfifo(tcb), pb->pb_data + pb->pb_offset + skip,
                chunk, seq, tcb->tcb_rxfifo_size);
    seq += chunk;
    len -= chunk;
    skip = 0;
  }
}


// Keep a segment that lies behind a hole, as much of it as is inside
// the window. Returns 0 if nothing was kept
static int
tcp_ooo_store(tcb_t *tcb, const pbuf_t *pb, size_t hdr_len, uint32_t seq,
              int fin, uint32_t rcv_wnd)
{
  const uint32_t off = seq - tcb->tcb_rcv.nxt;
  uint32_t len = pb->pb_pktlen - hdr_len;

  if(off + len + !!fin > rcv_wnd) {
    len = rcv_wnd - off;
    fin = 0;
  }

  if(len) {
    if(!tcp_blocks_insert(tcb->tcb_ooo, &tcb->tcb_ooo_count, TCP_OOO_BLOCKS,
                          tcb->tcb_rcv.nxt, seq, seq + len))
      return 0;
    tcb->tcb_ooo_recent = seq;
    tcp_rxfifo_store(tcb, pb, hdr_len, seq, len);
  }

  if(fin) {
    tcb->tcb_ooo_fin = 1;
    tcb->tcb_ooo_fin_seq = seq + len;
  }
  return len || fin;
}


// Segments we cannot take are answered with an ACK, which is what
// makes the peer retransmit, so we must never go quiet for good. But a
// burst of them needs only a few answers: at most five per 10 ms.
// Returns nonzero if an ACK may be sent
static int
tcp_reject_limit(tcb_t *tcb)
{
  const uint32_t now = clock_get() >> 10; // About ms
  if(now - tcb->tcb_reject_time >= 10) {
    tcb->tcb_reject_time = now;
    tcb->tcb_reject_count = 0;
  }
  if(tcb->tcb_reject_count == 5)
    return 0;
  tcb->tcb_reject_count++;
  return 1;
}


static void
tcp_disarm_all_timers(tcb_t *tcb)
{
  timer_disarm(&tcb->tcb_rtx_timer);
  timer_disarm(&tcb->tcb_delayed_ack_timer);
  timer_disarm(&tcb->tcb_time_wait_timer);
}


static void
tcp_close(tcb_t *tcb, const char *reason)
{
  tcp_disarm_all_timers(tcb);

  mutex_lock(&tcbs_mutex);
  LIST_REMOVE(tcb, tcb_link);
  mutex_unlock(&tcbs_mutex);

  tcp_set_state(tcb, TCP_STATE_CLOSED, reason);

  task_wakeup(&tcb->tcb_rx_waitq, 1);
  task_wakeup(&tcb->tcb_tx_waitq, 1);

  tcp_destroy(tcb);
}


// Make room for one more half-open connection, see TCP_MAX_HALF_OPEN
static void
tcp_half_open_limit(void)
{
  tcb_t *tcb, *oldest = NULL;
  int count = 0;

  // New ones go first in the list, so the last one found is the oldest
  LIST_FOREACH(tcb, &tcbs, tcb_link) {
    if(tcb->tcb_svc != NULL) {
      oldest = tcb;
      count++;
    }
  }
  if(count >= TCP_MAX_HALF_OPEN)
    tcp_close(oldest, "Too many half-open");
}


static void
tcp_time_wait_cb(void *opaque, uint64_t now)
{
  tcb_t *tcb = opaque;
  tcp_close(tcb, "time-wait timeout");
}


static void
tcp_time_wait(tcb_t *tcb, const char *reason)
{
  tcp_set_state(tcb, TCP_STATE_TIME_WAIT, reason);
  tcp_disarm_all_timers(tcb);
  net_timer_arm(&tcb->tcb_time_wait_timer, clock_get() + 5000000);
}


static void
tcp_delayed_ack_cb(void *opaque, uint64_t now)
{
  tcb_t *tcb = opaque;
  tcp_emit(tcb, NULL, tcb->tcb_snd.nxt, 1, "delayed-ack");
}


static void
tcp_parse_options(tcb_t *tcb, const uint8_t *buf, size_t len)
{
  while(len > 0) {

    if(buf[0] == 0)
      return;
    if(buf[0] == 1) {
      buf++;
      len--;
      continue;
    }
    if(len < 2)
      return;
    int opt = buf[0];
    int optlen = buf[1];

    // (A length below 2 would not move us forward)
    if(optlen < 2 || optlen > len)
      return;

    switch(opt) {
    case 2:
      if(optlen == 4) {
        // Segments are cut to this size, so keep it sane
        tcb->tcb_max_segment_size = MAX(buf[3] | (buf[2] << 8), 64);
      }
      break;
    case 4:
      // SACK-permitted. We always offer it, so this is the agreement
      if(optlen == 2)
        tcb->tcb_sack_ok = 1;
      break;
    case 3:
      // RFC 7323 Window Scale option. Length must be 3.
      if(optlen == 3) {
        uint8_t shift = buf[2];
        // RFC caps shift at 14 to keep the maximum window under 2^30.
        if(shift > 14)
          shift = 14;
        tcb->tcb_snd_wnd_shift = shift;
        tcb->tcb_wnd_scale_ok = 1;
      }
      break;
    }
    buf += optlen;
    len -= optlen;
  }
}


static ssize_t
tcb_rxfifo_user_bytes(tcb_t *tcb)
{
  if(tcb->tcb_state == TCP_STATE_CLOSED)
    return ERR_NOT_CONNECTED;

  size_t used = tcb_rxfifo_used(tcb);

  if(tcb->tcb_fin_received) {
    if(used <= 1)
      return ERR_NOT_CONNECTED;
    used--;
  }
  return used;
}


static void
tcp_rxfifo_consume(tcb_t *tcb, int bytes)
{
  tcb->tcb_rcv.rdptr += bytes;

  if(!tcb->rcv_window_closed)
    return;

  // If our RX buffer have opened up enough, send ACK immediatly.
  // Enough is half the FIFO or a full segment, whichever is less
  // (RFC 9293 3.8.6.2.2), so that it can be reached on a small FIFO

  int thres = MIN(tcb->tcb_rxfifo_size / 2, tcb->tcb_rcv.mss);

  if(tcb_rxfifo_avail(tcb) < thres)
    return;

  tcb->rcv_window_closed = 0;
  net_task_raise(&tcb->tcb_task, TCP_EVENT_WND_OPEN);
}

static ssize_t
tcp_stream_read(stream_t *s, void *buf, size_t size, size_t require)
{
  tcb_t *tcb = (tcb_t *)s;
  size_t total = 0;

  int q = irq_forbid(IRQ_LEVEL_SWITCH);

  while(size) {

    ssize_t used = tcb_rxfifo_user_bytes(tcb);
    if(used < 0) {
      irq_permit(q);
      // End of stream. What we already copied comes first, the caller
      // gets the error on its next read
      return total ? total : used;
    }

    if(used == 0) {

      if(total >= require)
        break;
      task_sleep(&tcb->tcb_rx_waitq);
      continue;
    }

    size_t to_copy = MIN(size, used);
    wtol_memcpy(buf + total, tcb_rxfifo(tcb),
                to_copy, tcb->tcb_rcv.rdptr,
                tcb->tcb_rxfifo_size);

    tcp_rxfifo_consume(tcb, to_copy);
    total += to_copy;
    size -= to_copy;
  }

  irq_permit(q);
  return total;
}


static ssize_t
tcp_stream_writev(struct stream *s, struct iovec *iov, size_t iovcnt,
                  int flags)
{
  tcb_t *tcb = (tcb_t *)s;
  size_t written = 0;

  if(tcb->tcb_state == TCP_STATE_CLOSED)
    return ERR_NOT_CONNECTED;

  size_t total = 0;
  for(size_t i = 0; i < iovcnt; i++) {
    total += iov[i].iov_len;
  }

  mutex_lock(&tcb->tcb_write_mutex);

  int q = irq_forbid(IRQ_LEVEL_SWITCH);

  if(flags & STREAM_WRITE_ALL && tcb_txfifo_avail(tcb) < total) {
    irq_permit(q);
    mutex_unlock(&tcb->tcb_write_mutex);
    return 0;
  }

  for(size_t i = 0; i < iovcnt; i++) {
    size_t size = iov[i].iov_len;
    void *buf = iov[i].iov_base;
    while(size) {
      size_t avail = tcb_txfifo_avail(tcb);
      if(avail == 0) {
        if(flags & STREAM_WRITE_NO_WAIT || tcb->tcb_state == TCP_STATE_CLOSED) {
          irq_permit(q);
          mutex_unlock(&tcb->tcb_write_mutex);
          return written;
        }
        net_task_raise(&tcb->tcb_task, TCP_EVENT_EMIT);
        task_sleep(&tcb->tcb_tx_waitq);
        continue;
      }
      size_t to_copy = MIN(size, avail);
      ltow_memcpy(tcb_txfifo(tcb), buf,
                  to_copy, tcb->tcb_snd.wrptr,
                  tcb->tcb_txfifo_size);

      tcb->tcb_snd.wrptr += to_copy;
      buf += to_copy;
      written += to_copy;
      size -= to_copy;
    }
  }

  uint32_t unsent = tcb->tcb_snd.wrptr - tcb->tcb_snd.nxt;
  int pipeline_idle = (tcb->tcb_snd.una == tcb->tcb_snd.nxt);
  irq_permit(q);
  mutex_unlock(&tcb->tcb_write_mutex);

  // Nagle's algorithm: kick the TCP task to emit only if we either have
  // a full segment's worth of unsent data, or the pipeline is empty (no
  // unACKed data in flight — safe to send a small segment without
  // starving the wire). Partial-segment tails on an active pipeline are
  // held by the emit loop below and flushed when the next ACK arrives.
  if(unsent >= tcp_snd_mss(tcb) || pipeline_idle)
    net_task_raise(&tcb->tcb_task, TCP_EVENT_EMIT);

  return written;
}


static ssize_t
tcp_stream_write(stream_t *s, const void *buf, size_t size, int flags)
{
  tcb_t *tcb = (tcb_t *)s;
  struct iovec iov;

  if(buf == NULL) {
    net_task_raise(&tcb->tcb_task, TCP_EVENT_FLUSH);
    return 0;
  }

  iov.iov_base = (void *)buf;
  iov.iov_len = size;
  return tcp_stream_writev(s, &iov, 1, flags);
}


static void
tcp_stream_close(stream_t *s)
{
  tcb_t *tcb = (tcb_t *)s;
  net_task_raise(&tcb->tcb_task, TCP_EVENT_FLUSH);
  net_task_raise(&tcb->tcb_task, TCP_EVENT_CLOSE);
}


static task_waitable_t *
tcp_stream_poll(stream_t *s, poll_type_t type)
{
  tcb_t *tcb = (tcb_t *)s;
  if(type == POLL_STREAM_READ) {
    if(tcb_rxfifo_user_bytes(tcb))
      return NULL;
    return &tcb->tcb_rx_waitq;
  } else {
    if(tcb_txfifo_avail(tcb))
      return NULL;
    return &tcb->tcb_tx_waitq;
  }
}


static ssize_t
tcp_stream_peek(struct stream *s, void **buf, int wait)
{
  tcb_t *tcb = (tcb_t *)s;

  ssize_t used;

  int q = irq_forbid(IRQ_LEVEL_SWITCH);

  while(1) {
    used = tcb_rxfifo_user_bytes(tcb);

    if(used == 0 && wait) {
      task_sleep(&tcb->tcb_rx_waitq);
      continue;
    }

    if(used < 1) {
      irq_permit(q);
      return used;
    }
    break;
  }

  const size_t size = tcb->tcb_rxfifo_size;
  const size_t mask = size - 1;
  const size_t offset = tcb->tcb_rcv.rdptr & mask;

  *buf = tcb_rxfifo(tcb) + offset;

  irq_permit(q);

  if(used + offset > tcb->tcb_rxfifo_size)
    return tcb->tcb_rxfifo_size - offset;
  return used;
}

ssize_t
tcp_stream_drop(struct stream *s, size_t bytes)
{
  tcb_t *tcb = (tcb_t *)s;
  ssize_t used = tcb_rxfifo_user_bytes(tcb);
  if(used < 1)
    return used;

  assert(bytes <= used);

  tcp_rxfifo_consume(tcb, bytes);
  return bytes;
}


static const stream_vtable_t tcp_stream_vtable = {
  .read = tcp_stream_read,
  .write = tcp_stream_write,
  .writev = tcp_stream_writev,
  .close = tcp_stream_close,
  .poll = tcp_stream_poll,
  .peek = tcp_stream_peek,
  .drop = tcp_stream_drop
};



static tcb_t *
tcb_create(const char *name, size_t txfifo_size, size_t rxfifo_size)
{
  tcb_t *tcb = xalloc(sizeof(tcb_t) + txfifo_size + rxfifo_size, 0,
                      MEM_MAY_FAIL);
  if(tcb == NULL)
    return NULL;

  memset(tcb, 0, sizeof(tcb_t));

  tcb->tcb_txfifo_size = txfifo_size;
  tcb->tcb_rxfifo_size = rxfifo_size;

  tcb->tcb_stream.vtable = &tcp_stream_vtable;

  tcb->tcb_name = name;

  tcb->tcb_task.nt_cb = tcp_task_cb;

  tcb->tcb_rto = TCP_RTO_MIN;
  tcb->tcb_srtt = -1;

  tcb->tcb_time_wait_timer.t_cb = tcp_time_wait_cb;
  tcb->tcb_time_wait_timer.t_opaque = tcb;
  tcb->tcb_time_wait_timer.t_name = "tcp";

  tcb->tcb_delayed_ack_timer.t_cb = tcp_delayed_ack_cb;
  tcb->tcb_delayed_ack_timer.t_opaque = tcb;
  tcb->tcb_delayed_ack_timer.t_name = "tcp";

  tcb->tcb_rtx_timer.t_cb = tcp_rtx_cb;
  tcb->tcb_rtx_timer.t_opaque = tcb;
  tcb->tcb_rtx_timer.t_name = "tcp";

  tcb->tcb_rcv.mss = 1460;

  tcb->tcb_iss = tcp_initial_seq();
  tcb->tcb_snd.nxt = tcb->tcb_iss;
  tcb->tcb_snd.una = tcb->tcb_iss;
  tcb->tcb_snd.wrptr = tcb->tcb_iss + 1;

  tcb->tcb_max_segment_size = 536;

  tcb->tcb_timo = TCP_TIMEOUT_HANDSHAKE * 1000;

  task_waitable_init(&tcb->tcb_rx_waitq, "tcp");
  task_waitable_init(&tcb->tcb_tx_waitq, "tcp");

  mutex_init(&tcb->tcb_write_mutex, "tcp");
  return tcb;
}




struct pbuf *
tcp_input_ipv4(struct netif *ni, struct pbuf *pb, int tcp_offset)
{
  const ipv4_header_t *ip = pbuf_data(pb, 0);
  if(ip->dst_addr != ni->ni_ipv4_local_addr) {
    // XXX: counter
    return pb; // Not for us
  }

  if(!ipv4_cksum_verify(ni, pb, tcp_offset, IPPROTO_TCP)) {
    atomic_inc(&ni->ni_tcp_bad_cksum);
    return pb;
  }

  uint32_t remote_addr = ip->src_addr;
  pb = pbuf_drop(pb, tcp_offset, 0);
  if(pbuf_pullup(pb, sizeof(tcp_hdr_t))) {
    // XXX: counter
    return pb;
  }
  const tcp_hdr_t *th = pbuf_data(pb, 0);

  const uint32_t hdr_len = (th->off & 0xf0) >> 2;

  if(hdr_len > pb->pb_pktlen || hdr_len < sizeof(tcp_hdr_t)) {
    // XXX: counter
    return pb;
  }

  uint32_t seg_len = pb->pb_pktlen - hdr_len;

  if(th->flg & TCP_F_SYN)
    seg_len++;
  if(th->flg & TCP_F_FIN)
    seg_len++;

  const uint16_t local_port = th->dst_port;
  const uint16_t remote_port = th->src_port;

  tcb_t *tcb = tcb_find(remote_addr, remote_port, local_port);

  const uint32_t seq = ntohl(th->seq);
  const uint32_t ack = ntohl(th->ack);
  const uint16_t wnd = ntohs(th->wnd);
  const uint8_t flag = th->flg;

  if(tcb == NULL) {

    if(flag != TCP_F_SYN) {

      if(flag & TCP_F_RST)
        return pb;

      if(flag & TCP_F_ACK) {
        return tcp_reply(ni, pb, remote_addr, ack, 0, TCP_F_RST, 0);
      } else {
        return tcp_reply(ni, pb, remote_addr, 0, seq + seg_len,
                         TCP_F_RST | TCP_F_ACK, 0);
      }
    }

    if(local_port == 0)
      return pb;

    uint16_t local_port_ho = ntohs(local_port);

    const service_t *svc = service_find_by_ip_port(local_port_ho);

    if(svc == NULL || svc->open_stream == NULL) {
      return tcp_reject(ni, pb, remote_addr, local_port_ho, seq + 1,
                        "no service");
    }

    size_t txfifo_size =
      svc->txfifo_size_log2 ? 1u << svc->txfifo_size_log2 : 4096;
    size_t rxfifo_size =
      svc->rxfifo_size_log2 ? 1u << svc->rxfifo_size_log2 : 2048;
    tcp_half_open_limit();

    tcb_t *tcb = tcb_create(svc->name, txfifo_size, rxfifo_size);
    if(tcb == NULL) {
      return tcp_reject(ni, pb, remote_addr, local_port_ho, seq + 1,
                        "no memory");
    }

    // No application yet. If the handshake is never completed the tcb
    // is ours alone to free
    tcb->tcb_svc = svc;
    tcb->tcb_app_closed = 1;

    tcb->tcb_local_addr = ni->ni_ipv4_local_addr;
    tcb->tcb_remote_addr = remote_addr;
    tcp_set_netif(tcb, ni);

    tcb->tcb_local_port = local_port;
    tcb->tcb_remote_port = remote_port;

    tcb->tcb_rcv.nxt = seq + 1;
    tcb->tcb_rcv.rdptr = seq + 1;

    // The handshake timeout counts from here. (Left at zero, the first
    // retransmission timeout would give up on the connection instead
    // of sending the SYN-ACK again)
    tcb->tcb_last_rx = clock_get();

    if(!pbuf_pullup(pb, hdr_len)) {
      tcp_parse_options(tcb,
                        pbuf_data(pb, sizeof(tcp_hdr_t)),
                        hdr_len - sizeof(tcp_hdr_t));
    }

    tcp_send_syn(tcb, TCP_F_SYN | TCP_F_ACK, pb);

    tcp_set_state(tcb, TCP_STATE_SYN_RECEIVED, "syn-recvd");
    mutex_lock(&tcbs_mutex);
    LIST_INSERT_HEAD(&tcbs, tcb, tcb_link);
    mutex_unlock(&tcbs_mutex);
    return NULL;
  }

  const int una_ack = ack - tcb->tcb_snd.una;
  const int ack_nxt = tcb->tcb_snd.nxt - ack;
  if(tcb->tcb_state == TCP_STATE_SYN_SENT) {

    int ack_acceptance = 0;

    // 3.10.7.3 SYN-SENT STATE

    // First check ACK bit
    if(flag & TCP_F_ACK) {
      int ack_iss = ack - tcb->tcb_iss;
      int ack_nxt = ack - tcb->tcb_snd.nxt;

      if(ack_iss <= 0 || ack_nxt > 0) {
        if(flag & TCP_F_RST) {
          return pb;
        }
        return tcp_reply(ni, pb, remote_addr, tcb->tcb_snd.nxt,
                         0, TCP_F_RST, 0);
      }

      ack_acceptance = una_ack > 0 && ack_nxt >= 0;
    }

    if(flag & TCP_F_RST) {
      if(ack_acceptance) {
        tcp_close(tcb, "Connection refused");
        return pb;
      }
      return pb;
    }

    if(flag & TCP_F_SYN) {

      tcb->tcb_last_rx = clock_get();

      tcb->tcb_rcv.nxt = seq + 1;
      tcb->tcb_rcv.rdptr = seq + 1;

      // The peer's MSS and window scale. Our SYN offered window
      // scaling; if the peer's SYN doesn't, the shift stays 0 and
      // neither side scales (RFC 7323 section 2.2)
      if(!pbuf_pullup(pb, hdr_len)) {
        tcp_parse_options(tcb,
                          pbuf_data(pb, sizeof(tcp_hdr_t)),
                          hdr_len - sizeof(tcp_hdr_t));
      }

      if(flag & TCP_F_ACK) {
        tcp_ack(tcb, una_ack);
      }

      const int una_iss = tcb->tcb_snd.una - tcb->tcb_iss;

      tcb->tcb_snd.wnd = wnd;
      tcb->tcb_snd.wl1 = seq;
      tcb->tcb_snd.wl2 = ack;

      if(una_iss > 0) {
        tcp_established(tcb, "syn-ack-in-syn-sent");
        tcp_emit(tcb, pb, tcb->tcb_snd.nxt, 1, "ack-in-syn-recvd");
        return NULL;
      }

      tcp_send_syn(tcb, TCP_F_SYN | TCP_F_ACK, pb);
      tcp_set_state(tcb, TCP_STATE_SYN_RECEIVED, "syn-recvd");
      return NULL;
    }

    // Neither SYN or RST is set, drop segment
    return pb;
  }


  //
  // Step 1: Sequence acceptance check
  //
  // A segment falls in one of three classes:
  //
  //  - Taken: it continues at rcv.nxt. It may begin in data we already
  //    have (a retransmission that also carries new data), the old
  //    part is cut off. It may also be more than fits the window, see
  //    step 8.
  //
  //  - Acceptable but not taken: it lies behind a hole, or the window
  //    is closed (a zero window probe). RST and ACK are processed.
  //    Data behind a hole is put aside (see the comment at the top)
  //    and we answer with an ACK for rcv.nxt, the duplicate ACK the
  //    peer's fast retransmit runs on.
  //
  //  - Not acceptable: old duplicates and anything outside the window.
  //    Answered with an ACK and otherwise ignored (RFC 9293 3.10.7.4).
  //    Keepalive probes end up here on purpose.
  //

  const int nxt_seq = seq - tcb->tcb_rcv.nxt;
  const uint32_t rcv_wnd = tcb_rxfifo_avail(tcb);
  uint32_t trim_front = 0;
  int acceptance;
  int take;

  if(seg_len == 0) {
    acceptance = nxt_seq == 0 || (uint32_t)nxt_seq < rcv_wnd;
    take = 1;
  } else if(flag & TCP_F_SYN) {
    acceptance = nxt_seq == 0 && rcv_wnd;
    take = 1;
  } else if(nxt_seq <= 0) {
    trim_front = -nxt_seq;
    acceptance = trim_front < seg_len;
    take = rcv_wnd != 0;
  } else {
    acceptance = (uint32_t)nxt_seq < rcv_wnd;
    take = 0;
  }

  if(!acceptance) {
    if(flag & TCP_F_RST)
      return pb;

    if(!tcp_reject_limit(tcb))
      return pb;

    return tcp_reply(ni, pb, remote_addr, tcb->tcb_snd.nxt,
                     tcb->tcb_rcv.nxt, TCP_F_ACK, rcv_wnd);
  }

  //
  // Step 2: RST
  //
  // Only one that hits rcv.nxt exactly resets the connection. Others
  // in the window get an ACK (RFC 5961 section 3)
  //

  if(flag & TCP_F_RST) {
    if(nxt_seq == 0) {
      tcp_close(tcb, "Got RST");
      return pb;
    }
    if(!tcp_reject_limit(tcb))
      return pb;
    return tcp_reply(ni, pb, remote_addr, tcb->tcb_snd.nxt,
                     tcb->tcb_rcv.nxt, TCP_F_ACK, rcv_wnd);
  }

  //
  // Step 3: Security
  //

  //
  // Step 4: SYN Processing
  //

  // A SYN on a connection that exists is answered with an ACK and
  // otherwise ignored (RFC 5961 section 4). If it is from the peer,
  // restarted, the peer resets us in reply to that ACK. If it is
  // forged, the peer just drops the ACK. Sending a RST ourselves, as
  // the original rule has it, would let anyone who can guess the
  // sequence number have us reset the peer.
  if(flag & TCP_F_SYN) {
    if(!tcp_reject_limit(tcb))
      return pb;
    return tcp_reply(ni, pb, remote_addr, tcb->tcb_snd.nxt,
                     tcb->tcb_rcv.nxt, TCP_F_ACK, rcv_wnd);
  }

  //
  // Step 5: ACK Processing
  //

  if(!(flag & TCP_F_ACK)) {
    return pb;
  }

  tcb->tcb_last_rx = clock_get();

  // Scaled peer window. This is the first non-SYN segment (or later),
  // so RFC 7323 says apply the shift negotiated at handshake time.
  const uint32_t scaled_wnd = (uint32_t)wnd << tcb->tcb_snd_wnd_shift;

  switch(tcb->tcb_state) {
  case TCP_STATE_SYN_RECEIVED:
    // It must ack our SYN and nothing that we have not sent
    if(una_ack > 0 && ack_nxt >= 0) {
      if(tcp_established(tcb, "ack-in-syn-recvd")) {
        // The service would not take it
        tcp_reply(ni, pb, remote_addr, ack, 0, TCP_F_RST, 0);
        tcp_close(tcb, "Service refused");
        return NULL;
      }
      tcb->tcb_snd.wnd = scaled_wnd;
      tcb->tcb_snd.wl1 = seq;
      tcb->tcb_snd.wl2 = ack;
    } else {
      return tcp_reply(ni, pb, remote_addr, ack, 0, TCP_F_RST, 0);
    }
    // FALLTHRU
  case TCP_STATE_ESTABLISHED:
  case TCP_STATE_FIN_WAIT1:
  case TCP_STATE_FIN_WAIT2:
  case TCP_STATE_CLOSE_WAIT:
  case TCP_STATE_CLOSING:
  case TCP_STATE_LAST_ACK:
    if(una_ack >= 0 && ack_nxt >= 0) {

      int sack_new = 0;
      if(tcb->tcb_sack_ok && hdr_len > sizeof(tcp_hdr_t) &&
         !pbuf_pullup(pb, hdr_len)) {
        sack_new = tcp_sack_input(tcb, pbuf_data(pb, sizeof(tcp_hdr_t)),
                                  hdr_len - sizeof(tcp_hdr_t), ack);
      }

      // (ACKs that repeat a closed window answer our probes, they
      // don't say that anything was lost)
      const int dup = una_ack == 0 &&
        tcb->tcb_snd.una != tcb->tcb_snd.nxt &&
        (sack_new || (seg_len == 0 && scaled_wnd == tcb->tcb_snd.wnd &&
                      scaled_wnd != 0));

      tcp_ack(tcb, una_ack); // Handles una_ack=0 by doing nothing

      if(dup || sack_new)
        tcp_dup_ack(tcb, dup);

      int wl1_seq = seq - tcb->tcb_snd.wl1;
      int wl2_ack = ack - tcb->tcb_snd.wl2;

      // Take the window from the segment if it is newer than the one
      // we took it from last. One that acks new data always is, even
      // with an old sequence number (a retransmission): to keep the
      // old window then would move its right edge along with snd.una,
      // beyond anything the peer has offered
      if(una_ack > 0 || wl1_seq > 0 || (wl1_seq == 0 && wl2_ack >= 0)) {
        const int opened = scaled_wnd > tcb->tcb_snd.wnd;
        const int was_closed = tcb->tcb_snd.wnd == 0;
        tcb->tcb_snd.wnd = scaled_wnd;
        tcb->tcb_snd.wl1 = seq;
        tcb->tcb_snd.wl2 = ack;

        // More room: send what was waiting for it. If a zero window
        // probe is in flight the peer has dropped it, send it again
        // right away
        if(opened && tcb->tcb_snd.wrptr != tcb->tcb_snd.nxt)
          net_task_raise(&tcb->tcb_task, TCP_EVENT_EMIT);
        if(opened && was_closed && tcb->tcb_snd.una != tcb->tcb_snd.nxt)
          tcp_emit(tcb, NULL, tcb->tcb_snd.una, 0, "wnd-open-rtx");
      }

    } else if(una_ack < 0) {
      // Old. A segment that is merely late acks something we sent not
      // long ago, and no more than the TX FIFO holds was ever unacked.
      // One that acks something from before that is not the peer's
      // (RFC 5961 section 5): this is what makes data injected by
      // someone who guessed the receive window fail
      if((uint32_t)-una_ack > tcb->tcb_txfifo_size + 2) {
        if(!tcp_reject_limit(tcb))
          return pb;
        return tcp_reply(ni, pb, remote_addr, tcb->tcb_snd.nxt,
                         tcb->tcb_rcv.nxt, TCP_F_ACK, rcv_wnd);
      }
    } else if(ack_nxt < 0) {
      // Too new
      return tcp_reply(ni, pb, remote_addr, tcb->tcb_snd.nxt,
                       tcb->tcb_rcv.nxt, TCP_F_ACK, rcv_wnd);
    }

    // Has our FIN been acked? It is the last thing in the TX FIFO, and
    // it may not even have been sent yet if data is waiting before it
    if(tcb->tcb_snd.una != tcb->tcb_snd.wrptr)
      break;

    switch(tcb->tcb_state) {
    case TCP_STATE_FIN_WAIT1:
      tcp_set_state(tcb, TCP_STATE_FIN_WAIT2, "ACK in FIN_WAIT1");
      break;
    case TCP_STATE_CLOSING:
      tcp_time_wait(tcb, "ACK in CLOSING");
      break;
    case TCP_STATE_LAST_ACK:
      // Frees the tcb, the application closed it already
      tcp_close(tcb, "LAST_ACK");
      return pb;
    }
    break;

  case TCP_STATE_TIME_WAIT:
    tcp_set_state(tcb, TCP_STATE_TIME_WAIT, "ACK_IN_TIME_WAIT");
    break;
  }

  //
  // Step 7: URG bit (Ignored)
  //

  //
  // Step 8: Process payload
  //

  if(!take && nxt_seq > 0) {
    // Behind a hole
    switch(tcb->tcb_state) {
    case TCP_STATE_ESTABLISHED:
    case TCP_STATE_FIN_WAIT1:
    case TCP_STATE_FIN_WAIT2:
      break;
    default:
      return pb;
    }

    // One duplicate ACK for each segment we keep, and none for one we
    // don't: a peer without SACK counts them to tell how much got
    // through
    if(!tcp_ooo_store(tcb, pb, hdr_len, seq, flag & TCP_F_FIN, rcv_wnd))
      return pb;

    // Repeat the window of our last segment even if the application
    // has read since: an ACK with a changed window is a window update
    // to the peer, not a duplicate ACK (RFC 5681 section 2), and would
    // not count towards its fast retransmit
    return tcp_reply_dup_ack(tcb, pb, MIN(rcv_wnd, tcb->tcb_rcv.adv_wnd));
  }

  if(!take) {
    // Window closed
    if(!tcp_reject_limit(tcb))
      return pb;
    return tcp_reply(ni, pb, remote_addr, tcb->tcb_snd.nxt,
                     tcb->tcb_rcv.nxt, TCP_F_ACK, rcv_wnd);
  }

  int fin = flag & TCP_F_FIN;

  pb = pbuf_drop(pb, hdr_len + trim_front, 0);

  switch(tcb->tcb_state) {
  case TCP_STATE_ESTABLISHED:
  case TCP_STATE_FIN_WAIT1:
  case TCP_STATE_FIN_WAIT2:

    if(!pb->pb_pktlen)
      break;

    {
      // Take what fits the window (rcv_wnd is not zero here). A FIN
      // occupies a byte of the FIFO as well; one that does not fit is
      // left for the peer to send again together with the rest.
      uint32_t len = pb->pb_pktlen;
      if(len + !!fin > rcv_wnd) {
        len = MIN(len, rcv_wnd);
        fin = 0;
      }

      tcp_rxfifo_store(tcb, pb, 0, tcb->tcb_rcv.nxt, len);
      tcb->tcb_rcv.nxt += len;
      tcb->tcb_rx_bytes += len;

      // If this filled a hole, what was waiting behind it comes along,
      // possibly with a FIN, and the peer is told right away
      const int filled_hole = tcb->tcb_ooo_count || tcb->tcb_ooo_fin;
      tcp_ooo_advance(tcb);
      if(tcb->tcb_ooo_fin) {
        const int32_t d = tcb->tcb_ooo_fin_seq - tcb->tcb_rcv.nxt;
        if(d <= 0) {
          tcb->tcb_ooo_fin = 0;
          if(d == 0 && tcb_rxfifo_avail(tcb))
            fin = 1;
        }
      }

      task_wakeup(&tcb->tcb_rx_waitq, 1);

      /*
        Ack immediately if we have nothing to send and
          We are waiting to send a delayed ack (which will be cancelled)
         OR
          Our RX buffer is fully depleted
      */
      int rx_avail = tcb_rxfifo_avail(tcb);
      tcb->rcv_window_closed = rx_avail < tcb->tcb_rcv.mss;

      if(filled_hole ||
         (tcb->tcb_snd.wrptr == tcb->tcb_snd.nxt &&
          (!timer_disarm(&tcb->tcb_delayed_ack_timer) ||
           tcb->rcv_window_closed))) {
        tcp_emit(tcb, pb, tcb->tcb_snd.nxt, 1, "Instant ack");
        pb = NULL;
      } else {
        net_timer_arm(&tcb->tcb_delayed_ack_timer,
                      tcb->tcb_last_rx + 20000);
      }
    }
    break;

  default:
    break;
  }

  if(fin) {

    switch(tcb->tcb_state) {
    case TCP_STATE_CLOSED:
    case TCP_STATE_LISTEN:
    case TCP_STATE_SYN_SENT:
      return pb;
    default:
      break;
    }

    tcb->tcb_fin_received = 1;
    task_wakeup(&tcb->tcb_rx_waitq, 1);
    tcb->tcb_rcv.nxt++;
    tcp_emit(tcb, pb, tcb->tcb_snd.nxt, 1, "FIN-response");
    pb = NULL;

    switch(tcb->tcb_state) {
    case TCP_STATE_SYN_RECEIVED:
    case TCP_STATE_ESTABLISHED:
      tcp_set_state(tcb, TCP_STATE_CLOSE_WAIT, "FIN in ESTABLISHED");
      break;
    case TCP_STATE_FIN_WAIT1:
      if(tcb->tcb_snd.una == tcb->tcb_snd.wrptr) {
        tcp_time_wait(tcb, "FIN in FIN_WAIT1");
      } else {
        tcp_set_state(tcb, TCP_STATE_CLOSING, "FIN in FIN_WAIT1");
      }
      break;
    case TCP_STATE_FIN_WAIT2:
      tcp_time_wait(tcb, "FIN in FIN_WAIT2");
      break;
    case TCP_STATE_CLOSE_WAIT:
    case TCP_STATE_CLOSING:
    case TCP_STATE_LAST_ACK:
      break;
    }
  }


  return pb;
}


struct stream *
tcp_create_socket(const char *name, size_t txfifo_size, size_t rxfifo_size)
{
  txfifo_size = round_down_pow2(txfifo_size);
  rxfifo_size = round_down_pow2(rxfifo_size);

  txfifo_size = MAX(txfifo_size, 2048);
  rxfifo_size = MAX(rxfifo_size, 2048);

  tcb_t *tcb = tcb_create(name, txfifo_size, rxfifo_size);
  if(tcb == NULL)
    return NULL;
  return &tcb->tcb_stream;
}


void
tcp_connect(struct stream *s, uint32_t dst_addr, uint16_t dst_port)
{
  tcb_t *tcb = (void *)s - offsetof(tcb_t, tcb_stream);

  tcp_set_state(tcb, TCP_STATE_SYN_SENT, "syn-sent");

  tcb->tcb_remote_addr = dst_addr;
  tcb->tcb_remote_port = htons(dst_port);

  net_task_raise(&tcb->tcb_task, TCP_EVENT_CONNECT);
}


static const char *tcp_statenames =
  "CLOSED\0"
  "LISTEN\0"
  "SYN_SENT\0"
  "SYN_RECEIVED\0"
  "ESTABLISHED\0"
  "FIN_WAIT1\0"
  "FIN_WAIT2\0"
  "CLOSING\0"
  "TIME_WAIT\0"
  "CLOSE_WAIT\0"
  "LAST_ACK\0";


static const char *
tcp_state_to_str(int state)
{
  return strtbl(tcp_statenames, state);
}


static error_t
cmd_tcp(cli_t *cli, int argc, char **argv)
{
  tcb_t *tcb;

  mutex_lock(&tcbs_mutex);

  LIST_FOREACH(tcb, &tcbs, tcb_link) {
    cli_printf(cli, "%s\n\tLocal: %Id:%-5d\tRemote: %Id:%-5d\n",
               tcb->tcb_name,
               tcb->tcb_local_addr,
               ntohs(tcb->tcb_local_port),
               tcb->tcb_remote_addr,
               ntohs(tcb->tcb_remote_port));
    cli_printf(cli, "\t%s TX:%"PRIu64" RX:%"PRIu64" ReTX:%"PRIu64"  Fast ReTX:%d  Probes:%d  Failed ReTX:%d\n",
               tcp_state_to_str(tcb->tcb_state),
               tcb->tcb_tx_bytes,
               tcb->tcb_rx_bytes,
               tcb->tcb_rtx_bytes,
               tcb->tcb_fast_rtx,
               tcb->tcb_tlp_count,
               tcb->tcb_rtx_drop);
    cli_printf(cli, "\tRTO: %d ms  SRTT: %d us  MSS: %d  Unacked bytes:%d%s",
               tcb->tcb_rto, tcb->tcb_srtt, (int)tcp_snd_mss(tcb),
               tcb->tcb_snd.nxt  - tcb->tcb_snd.una,
               tcb->tcb_sack_ok ? "  SACK" : "");
    cli_printf(cli, "\tRX-FIFO:%d  TX-FIFO:%d\n",
               tcb_rxfifo_used(tcb),
               tcb_txfifo_used(tcb));
  }
  mutex_unlock(&tcbs_mutex);
  return 0;
}

CLI_CMD_DEF_EXT("show_tcp", cmd_tcp, NULL, "Show active TCP connections");



static error_t
cmd_killtcp(cli_t *cli, int argc, char **argv)
{
  tcb_t *tcb;

  mutex_lock(&tcbs_mutex);

  LIST_FOREACH(tcb, &tcbs, tcb_link) {
    tcb->tcb_local_port = ~tcb->tcb_local_port;
  }
  mutex_unlock(&tcbs_mutex);
  return 0;
}


CLI_CMD_DEF("tcp_kill", cmd_killtcp);

