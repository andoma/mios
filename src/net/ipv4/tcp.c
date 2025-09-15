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

#define TCP_EVENT_CONNECT 0x1
#define TCP_EVENT_CLOSE   0x2
#define TCP_EVENT_EMIT    0x4
#define TCP_EVENT_WND_OPEN 0x8
/*
 * Based on these RFCs:
 *
 *  Transmission Control Protocol (TCP)
 *          https://datatracker.ietf.org/doc/html/rfc9293
 *
 *  Computing TCP's Retransmission Timer
 *          https://datatracker.ietf.org/doc/html/rfc6298
 *
 */


#define TCP_KEEPALIVE_INTERVAL 10000 // ms
#define TCP_TIMEOUT_INTERVAL   21000 // ms
#define TCP_TIMEOUT_HANDSHAKE   5000 // ms

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

  uint8_t tcb_pending_syn;
  uint8_t tcb_pending_fin;

  uint8_t tcb_fin_received;
  uint8_t rcv_window_closed;

  struct {
    uint32_t wrptr; // Unsent (Enqueued by stream but not yet processed by TCP)
    uint32_t nxt;   // Next to send
    uint32_t una;   // Unacknowledged
    uint16_t wnd;
    uint16_t up;
    uint32_t wl1;
    uint32_t wl2;
  } tcb_snd;
  uint32_t tcb_iss;

  struct {
    uint32_t rdptr;
    uint32_t nxt;
    uint16_t up;
    uint16_t mss;
  } tcb_rcv;

  uint32_t tcb_local_addr;
  uint32_t tcb_remote_addr;

  uint16_t tcb_remote_port;
  uint16_t tcb_local_port;

  uint16_t tcb_max_segment_size;

  uint16_t tcb_txfifo_size; // Must be power of 2
  uint16_t tcb_rxfifo_size; // Must be power of 2

  timer_t tcb_rtx_timer;
  timer_t tcb_delayed_ack_timer;
  timer_t tcb_time_wait_timer;

  int tcb_rto;  // in ms
  int tcb_srtt;
  int tcb_rttvar;

  uint64_t tcb_tx_bytes;
  uint64_t tcb_rx_bytes;
  uint64_t tcb_rtx_bytes;

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

  uint32_t end = (offset + length) & mask;

  if(end >= offset) {
    memcpy(dst, fifo + offset, length);
  } else {
    memcpy(dst, fifo + offset, size - offset);
    memcpy(dst + size - offset, fifo, end);
  }
}


static void
ltow_memcpy(uint8_t *fifo, const uint8_t *src,
            uint32_t length, uint32_t offset, size_t size)
{
  const size_t mask = size - 1;
  offset &= mask;
  uint32_t end = (offset + length) & mask;

  if(end >= offset) {
    memcpy(fifo + offset, src, length);
  } else {
    memcpy(fifo + offset, src, size - offset);
    memcpy(fifo, src + size - offset, end);
  }
}



#define TCP_PBUF_HEADROOM (16 + sizeof(ipv4_header_t) + sizeof(tcp_hdr_t))

static void tcp_close(tcb_t *tcb, const char *reason);

static const char *tcp_state_to_str(int state);


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


static error_t
tcp_output_tcb(tcb_t *tcb, pbuf_t *pb, const char *why)
{
  tcp_hdr_t *th = pbuf_data(pb, 0);

  th->src_port = tcb->tcb_local_port;
  th->dst_port = tcb->tcb_remote_port;
  th->ack = htonl(tcb->tcb_rcv.nxt);

  uint16_t rcv_wnd = tcb_rxfifo_avail(tcb);
  th->wnd = htons(rcv_wnd);

  if(th->flg & TCP_F_SYN) {
    uint8_t *opts = pbuf_append(pb, 4);
    opts[0] = 2;
    opts[1] = 4;
    opts[2] = tcb->tcb_rcv.mss >> 8;
    opts[3] = tcb->tcb_rcv.mss;
    th->off = ((sizeof(tcp_hdr_t) >> 2) + 1) << 4;
  } else {
    th->off = (sizeof(tcp_hdr_t) >> 2) << 4;
  }

  return tcp_output(pb, tcb->tcb_local_addr, tcb->tcb_remote_addr);
}


static error_t
tcp_emit(tcb_t *tcb, pbuf_t *pb, uint32_t seq, int gen_ack, const char *why)
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

  uint32_t bytes_in_fifo = tcb->tcb_snd.wrptr - seq;

  if(tcb->tcb_pending_syn) {
    // If we have a pending SYN it's the only thing we may send
    th->flg = tcb->tcb_pending_syn;
    th->seq = htonl(tcb->tcb_iss);
    return tcp_output_tcb(tcb, pb, "pending-syn");

  } else if(!bytes_in_fifo) {
    // Nothing in FIFO to send, but send an ACK if asked to
    if(gen_ack) {
      th->flg = TCP_F_ACK;
      th->seq = htonl(seq);
      return tcp_output_tcb(tcb, pb, "empty_ack");
    } else {
      pbuf_free(pb);
    }
    return 0;

  } else if(tcb->tcb_pending_fin) {

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

  size_t max_pkt_size = 1460 + sizeof(tcp_hdr_t);

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
  } else {
    tcb->tcb_rtx_bytes += total;
  }
  return 0;
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

  if(tcb->tcb_snd.una != tcb->tcb_snd.nxt) {
    rtx_duration_ms = tcb->tcb_rto;
  } else {
    rtx_duration_ms = TCP_KEEPALIVE_INTERVAL;
  }
  net_timer_arm(&tcb->tcb_rtx_timer, now + rtx_duration_ms * 1000);
}


static void
tcp_rtx_cb(void *opaque, uint64_t now)
{
  tcb_t *tcb = opaque;

  if(now > tcb->tcb_last_rx + tcb->tcb_timo) {
    tcp_close(tcb, "TCP Timeout");
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

  } else {
    tcp_emit(tcb, NULL, tcb->tcb_snd.una, 0, "RTX");
  }

  arm_rtx(tcb, now);
}


static void
tcp_ack(tcb_t *tcb, uint32_t count)
{
  if(count == 0)
    return;

  tcb->tcb_snd.una += count;
  tcb->tcb_pending_syn = 0;
  task_wakeup(&tcb->tcb_tx_waitq, 1);
  arm_rtx(tcb, tcb->tcb_last_rx);
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

  if(signals & TCP_EVENT_EMIT) {
    switch(tcb->tcb_state) {
    case TCP_STATE_ESTABLISHED:
    case TCP_STATE_FIN_WAIT1:
    case TCP_STATE_FIN_WAIT2:
    case TCP_STATE_CLOSE_WAIT:
    case TCP_STATE_CLOSING:
      if(tcb->tcb_snd.una == tcb->tcb_snd.nxt) {
        // Nothing currently outstanding, arm rtx timer
        net_timer_arm(&tcb->tcb_rtx_timer, clock_get() + tcb->tcb_rto * 1000);
      }

      while(tcb->tcb_snd.wrptr != tcb->tcb_snd.nxt) {
        if(tcp_emit(tcb, NULL, tcb->tcb_snd.nxt, 0, "event"))
          break;
      }
      break;

    default:
      break;
    }
  }

  if(signals & TCP_EVENT_CLOSE) {
    tcb->tcb_app_closed = 1;

    switch(tcb->tcb_state) {
    case TCP_STATE_ESTABLISHED:
    case TCP_STATE_SYN_RECEIVED:
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
          uint32_t seq, uint32_t ack, uint8_t flag, uint16_t wnd)
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
  th->wnd = htons(wnd);
  th->off = (sizeof(tcp_hdr_t) >> 2) << 4;

  tcp_output(pb, ni->ni_ipv4_local_addr, remote_addr);
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

    if(optlen > len)
      return;

    switch(opt) {
    case 2:
      tcb->tcb_max_segment_size = buf[3] | (buf[2] << 8);
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

  // If our RX buffer have opened up enough, send ACK immediatly

  int thres = MAX(tcb->tcb_rxfifo_size / 2, tcb->tcb_rcv.mss * 2);

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
      return used;
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

  irq_permit(q);
  mutex_unlock(&tcb->tcb_write_mutex);

  if(flags & STREAM_WRITE_ALL)
    net_task_raise(&tcb->tcb_task, TCP_EVENT_EMIT);

  return written;
}


static ssize_t
tcp_stream_write(stream_t *s, const void *buf, size_t size, int flags)
{
  tcb_t *tcb = (tcb_t *)s;
  struct iovec iov;

  if(buf == NULL) {
    net_task_raise(&tcb->tcb_task, TCP_EVENT_EMIT);
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
  net_task_raise(&tcb->tcb_task, TCP_EVENT_EMIT);
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

  tcb->tcb_rto = 250;

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
  //  tcb->tcb_rcv.wnd = tcb->tcb_rxfifo_size;

  tcb->tcb_iss = rand();
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

    tcb_t *tcb = tcb_create(svc->name, 4096, 2048);
    if(tcb == NULL) {
      return tcp_reject(ni, pb, remote_addr, local_port_ho, seq + 1,
                        "no memory");
    }

    tcb->tcb_local_addr = ni->ni_ipv4_local_addr;
    tcb->tcb_remote_addr = remote_addr;

    tcb->tcb_local_port = local_port;
    tcb->tcb_remote_port = remote_port;

    tcb->tcb_rcv.nxt = seq + 1;
    tcb->tcb_rcv.rdptr = seq + 1;

    if(!pbuf_pullup(pb, hdr_len)) {
      tcp_parse_options(tcb,
                        pbuf_data(pb, sizeof(tcp_hdr_t)),
                        hdr_len - sizeof(tcp_hdr_t));
    }

    error_t err = svc->open_stream(&tcb->tcb_stream);
    if(err) {
      free(tcb);
      return tcp_reject(ni, pb, remote_addr, local_port_ho, seq + 1,
                        error_to_string(err));
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

      if(flag & TCP_F_ACK) {
        tcp_ack(tcb, una_ack);
      }

      const int una_iss = tcb->tcb_snd.una - tcb->tcb_iss;

      tcb->tcb_snd.wnd = wnd;
      tcb->tcb_snd.wl1 = seq;
      tcb->tcb_snd.wl2 = ack;

      if(una_iss > 0) {
        tcb->tcb_timo = TCP_TIMEOUT_INTERVAL * 1000;
        tcp_set_state(tcb, TCP_STATE_ESTABLISHED, "ack-in-syn-recvd");
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

  int acceptance = 0;
  int nxt_seq = seq - tcb->tcb_rcv.nxt;
  uint32_t rcv_wnd = tcb_rxfifo_avail(tcb);
  if(seg_len == 0) {


    if(rcv_wnd == 0) {
      acceptance = nxt_seq == 0;
    } else {
      acceptance = (uint32_t)nxt_seq < tcb->tcb_rcv.nxt + rcv_wnd;
    }
  } else {

    if(rcv_wnd == 0) {

    } else {

#if 0
      acceptance = (uint32_t)nxt_seq < tcb->tcb_rcv.nxt + tcb->tcb_rcv.wnd;

      int seq_delta_end = nxt_seq + seg_len - 1;
      acceptance |=
        (uint32_t)seq_delta_end < tcb->tcb_rcv.nxt + tcb->tcb_rcv.wnd;
#endif

      acceptance = nxt_seq == 0;
    }
  }

  if(!acceptance) {
    if(flag & TCP_F_RST)
      return pb;
    return tcp_reply(ni, pb, remote_addr, tcb->tcb_snd.nxt,
                     tcb->tcb_rcv.nxt, TCP_F_ACK, rcv_wnd);
  }

  //
  // Step 2: RST
  //

  if(flag & TCP_F_RST) {
    tcp_close(tcb, "Got RST");
    return pb;
  }

  //
  // Step 3: Security
  //

  //
  // Step 4: SYN Processing
  //

  if(flag & TCP_F_SYN) {
    return tcp_reply(ni, pb, remote_addr, tcb->tcb_snd.nxt,
                     tcb->tcb_rcv.nxt, TCP_F_RST, 0);
  }

  //
  // Step 5: ACK Processing
  //

  if(!(flag & TCP_F_ACK)) {
    return pb;
  }

  tcb->tcb_last_rx = clock_get();

  switch(tcb->tcb_state) {
  case TCP_STATE_SYN_RECEIVED:
    if(una_ack >= 0 && ack_nxt <= 0) {
      tcb->tcb_timo = TCP_TIMEOUT_INTERVAL * 1000;
      tcp_set_state(tcb, TCP_STATE_ESTABLISHED, "ack-in-syn-recvd");
      tcb->tcb_snd.wnd = wnd;
      tcb->tcb_snd.wl1 = seq;
      tcb->tcb_snd.wl2 = ack;
    } else {
      return tcp_reply(ni, pb, remote_addr, tcb->tcb_snd.nxt,
                       tcb->tcb_rcv.nxt, TCP_F_RST, 0);
    }
    // FALLTHRU
  case TCP_STATE_ESTABLISHED:
  case TCP_STATE_FIN_WAIT1:
  case TCP_STATE_FIN_WAIT2:
  case TCP_STATE_CLOSE_WAIT:
  case TCP_STATE_CLOSING:
    if(una_ack >= 0 && ack_nxt >= 0) {

      tcp_ack(tcb, una_ack); // Handles una_ack=0 by doing nothing

      int wl1_seq = seq - tcb->tcb_snd.wl1;
      int wl2_ack = ack - tcb->tcb_snd.wl2;

      if(wl1_seq > 0 || (wl1_seq == 0 && wl2_ack >= 0)) {
        tcb->tcb_snd.wnd = wnd;
        tcb->tcb_snd.wl1 = seq;
        tcb->tcb_snd.wl2 = ack;
      }

    } else if(una_ack < 0) {
      // Old
    } else if(ack_nxt < 0) {
      // Too new
      return tcp_reply(ni, pb, remote_addr, tcb->tcb_snd.nxt,
                       tcb->tcb_rcv.nxt, TCP_F_ACK, rcv_wnd);
    }

    switch(tcb->tcb_state) {
    case TCP_STATE_FIN_WAIT1:
      if(ack_nxt == 0) {
        tcp_set_state(tcb, TCP_STATE_FIN_WAIT2, "ACK in FIN_WAIT1");
      }
      break;
    case TCP_STATE_FIN_WAIT2:
      break;
    case TCP_STATE_CLOSE_WAIT:
      break;
    case TCP_STATE_CLOSING:
      if(ack_nxt == 0) {
        tcp_set_state(tcb, TCP_STATE_FIN_WAIT2, "ACK in CLOSING");
        // arm timer
      }
    }
    break;

  case TCP_STATE_LAST_ACK:
    tcp_close(tcb, "LAST_ACK");
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

  pb = pbuf_drop(pb, hdr_len, 0);

  switch(tcb->tcb_state) {
  case TCP_STATE_ESTABLISHED:
  case TCP_STATE_FIN_WAIT1:
  case TCP_STATE_FIN_WAIT2:

    if(!pb->pb_pktlen)
      break;

    uint32_t avail = tcb_rxfifo_avail(tcb);

    if(avail >= pb->pb_pktlen) {

      pbuf_t *p;
      for(p = pb; p != NULL; p = p->pb_next) {
        ltow_memcpy(tcb_rxfifo(tcb), p->pb_data + p->pb_offset,
                    p->pb_buflen, tcb->tcb_rcv.nxt,
                    tcb->tcb_rxfifo_size);
        tcb->tcb_rcv.nxt += p->pb_buflen;
      }
      tcb->tcb_rx_bytes += pb->pb_pktlen;

      task_wakeup(&tcb->tcb_rx_waitq, 1);

      /*
        Ack immediately if we have nothing to send and
          We are waiting to send a delayed ack (which will be cancelled)
         OR
          Our RX buffer is fully depleted
      */
      int rx_avail = tcb_rxfifo_avail(tcb);
      tcb->rcv_window_closed = rx_avail < tcb->tcb_rcv.mss;

      if(tcb->tcb_snd.wrptr == tcb->tcb_snd.nxt &&
         (!timer_disarm(&tcb->tcb_delayed_ack_timer) ||
          tcb->rcv_window_closed)) {
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

  if(flag & TCP_F_FIN) {

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
      if(ack == tcb->tcb_snd.nxt) {
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
    cli_printf(cli, "\t%s TX:%"PRIu64" RX:%"PRIu64" ReTX:%"PRIu64"  Failed ReTX:%d\n",
               tcp_state_to_str(tcb->tcb_state),
               tcb->tcb_tx_bytes,
               tcb->tcb_rx_bytes,
               tcb->tcb_rtx_bytes,
               tcb->tcb_rtx_drop);
    cli_printf(cli, "\tRTO: %d ms  Unacked bytes:%d", tcb->tcb_rto,
               tcb->tcb_snd.nxt  - tcb->tcb_snd.una);
    cli_printf(cli, "\tRX-FIFO:%d  TX-FIFO:%d\n",
               tcb_rxfifo_used(tcb),
               tcb_txfifo_used(tcb));
  }
  mutex_unlock(&tcbs_mutex);
  return 0;
}

CLI_CMD_DEF("tcp", cmd_tcp);



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


CLI_CMD_DEF("killtcp", cmd_killtcp);

