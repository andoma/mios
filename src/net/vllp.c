#include <mios/vllp.h>

#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <malloc.h>
#include <assert.h>

#include <mios/dsig.h>
#include <mios/eventlog.h>
#include <mios/timer.h>
#include <mios/service.h>
#include <mios/stream.h>
#include <mios/cli.h>

#include <sys/param.h>

#include "util/crc32.h"
#include "net/pbuf.h"
#include "net/net_task.h"

#include "irq.h"

LIST_HEAD(vllp_list, vllp);
LIST_HEAD(vllp_channel_list, vllp_channel);
TAILQ_HEAD(vllp_channel_queue, vllp_channel);

static struct vllp_list vllps;

typedef struct vllp {

  LIST_ENTRY(vllp) link;

  struct vllp_channel_list channels;
  struct vllp_channel_queue established_channels;

  timer_t ack_timer;
  timer_t rtx_timer;
  timer_t timeout_timer;

  pbuf_t *current_tx_buf;
  uint16_t current_tx_len;
  uint8_t current_tx_channel;

  struct vllp_channel *cmc;

  uint32_t rxid;
  uint32_t txid;
  uint32_t crc_IV;
  uint32_t channel_iv_cnt;

  uint16_t remote_flow_status;
  uint16_t local_flow_status;
  uint16_t transmitted_local_flow_status;

  uint8_t connected;
  uint8_t SE;
  uint8_t mtu;
  uint8_t timeout;
} vllp_t;



#define VLLP_CHANNEL_STATE_PENDING     0
#define VLLP_CHANNEL_STATE_OPEN_SENT   1
#define VLLP_CHANNEL_STATE_ESTABLISHED 2
#define VLLP_CHANNEL_STATE_CLOSED_SENT 3


struct vllp_channel {
  net_task_t task;

  vllp_t *vllp;

  LIST_ENTRY(vllp_channel) link;
  TAILQ_ENTRY(vllp_channel) qlink;

  struct pbuf_queue rxq;
  struct pbuf_queue txq;

  pushpull_t pp;

  uint32_t tx_crc_IV;
  uint32_t rx_crc_IV;

  uint8_t id;
  uint8_t state;
  uint8_t app_closed;
  uint8_t net_closed;
};


#define VLLP_VERSION 2

#define VLLP_SYN   0x0f

#define VLLP_HDR_S 0x80
#define VLLP_HDR_E 0x40
#define VLLP_HDR_F 0x20
#define VLLP_HDR_L 0x10

#define VLLP_CMC_OPCODE_OPEN              0
#define VLLP_CMC_OPCODE_OPEN_RESPONSE     2
#define VLLP_CMC_OPCODE_CLOSE             3

static void vllp_channel_task_cb(net_task_t *nt, uint32_t signals);

static void __attribute__((unused))
logpkt(const pbuf_t *pb, const char *prefix)
{

  for(; pb != NULL ; pb = pb->pb_next) {
    evlog(LOG_DEBUG, "%s: PBUF %c%c (%d) %.*s",
          prefix,
          pb->pb_flags & PBUF_SOP ? 'S' : '-',
          pb->pb_flags & PBUF_EOP ? 'E' : '-',
          pb->pb_pktlen,
          -pb->pb_buflen,
          (const char *)pb->pb_data + pb->pb_offset);
  }
}


uint32_t
calc_crc32(struct pbuf *pb, uint32_t crc)
{
  for(; pb != NULL; pb = pb->pb_next)
    crc = crc32(crc, pb->pb_data + pb->pb_offset, pb->pb_buflen);

  return ~crc;
}

static void
vllp_append_crc(uint32_t iv, uint8_t *pkt, size_t len)
{
  uint32_t crc = ~crc32(iv, pkt, len);
  pkt[len + 0] = crc;
  pkt[len + 1] = crc >> 8;
  pkt[len + 2] = crc >> 16;
  pkt[len + 3] = crc >> 24;
}


static uint32_t
vllp_gen_channel_crc(vllp_t *v)
{
  v->channel_iv_cnt++;
  return crc32(v->crc_IV, &v->channel_iv_cnt, sizeof(v->channel_iv_cnt));
}


static void
vllp_channel_destroy(vllp_t *v, vllp_channel_t *vc)
{
  int q = irq_forbid(IRQ_LEVEL_NET);
  pbuf_free_queue_irq_blocked(&vc->txq);
  pbuf_free_queue_irq_blocked(&vc->rxq);
  irq_permit(q);
  evlog(LOG_DEBUG, "VLLP: channel %d closed", vc->id);
  free(vc);
}


static vllp_channel_t *
vllp_channel_make(vllp_t *v, int id)
{
  vllp_channel_t *vc = xalloc(sizeof(vllp_channel_t), 0,
                               MEM_MAY_FAIL | MEM_CLEAR);
  if(vc == NULL)
    return NULL;

  vc->task.nt_cb = vllp_channel_task_cb;

  vc->id = id;
  STAILQ_INIT(&vc->txq);
  STAILQ_INIT(&vc->rxq);
  LIST_INSERT_HEAD(&v->channels, vc, link);
  vc->vllp = v;
  return vc;
}

static vllp_channel_t *
vllp_channel_find(vllp_t *v, int channel_id)
{
  vllp_channel_t *vc;
  LIST_FOREACH(vc, &v->channels, link) {
    if(vc->id == channel_id)
      return vc;
  }
  return NULL;
}

static int
vllp_refresh_local_flow_status(vllp_t *v)
{
  uint16_t bits = 0xffff;

  vllp_channel_t *vc;
  LIST_FOREACH(vc, &v->channels, link) {
    if(vc == v->cmc)
      continue;

    if(vc->net_closed)
      continue;

    if(!vc->pp.app->may_push || !vc->pp.app->may_push(vc->pp.app_opaque))
      bits &= ~(1 << vc->id);
  }

  if(v->local_flow_status == bits)
    return 0;
  v->local_flow_status = bits;
  return 1;
}


static pbuf_t *
vllp_tx_ack(vllp_t *v, pbuf_t *pb)
{
  if(pb == NULL) {
    pb = pbuf_make(4, 0); /* offset: 4 bytes for ID prefix in dsig.c */
    if(pb == NULL)
      return NULL;
  } else {
    pbuf_reset(pb, 4, 0);
  }

  uint8_t *pkt = pbuf_append(pb, 7);
  pkt[0] = v->SE | 0x1f;
  pkt[1] = v->local_flow_status;
  pkt[2] = v->local_flow_status >> 8;

  v->transmitted_local_flow_status = v->local_flow_status;

  vllp_append_crc(v->crc_IV, pkt, 3);

  dsig_emit_pbuf(v->txid, pb);

  net_timer_arm(&v->ack_timer, clock_get() + 1000000);
  return NULL;
}


static int
vllp_channel_maybe_destroy(vllp_t *v, vllp_channel_t *vc)
{
  if(vc->app_closed == 2 && vc->net_closed) {
    vllp_channel_destroy(v, vc);
    return 1;
  }
  return 0;
}

static void
vllp_channel_net_close(vllp_t *v, vllp_channel_t *vc,
                       const char *reason)
{
  if(vc->net_closed)
    return;

  vc->pp.app->close(vc->pp.app_opaque, reason);
  vc->net_closed = 1;
  LIST_REMOVE(vc, link);
  vllp_channel_maybe_destroy(v, vc);
}


static void
vllp_disconnect(vllp_t *v, const char *reason)
{
  vllp_channel_t *vc, *n;

  if(!v->connected)
    return;

  timer_disarm(&v->ack_timer);
  timer_disarm(&v->rtx_timer);
  timer_disarm(&v->timeout_timer);

  evlog(LOG_DEBUG, "VLLP: 0x%x:0x%x Disconnected -- %s", v->txid, v->rxid, reason);

  for(vc = LIST_FIRST(&v->channels); vc != NULL; vc = n) {
    n = LIST_NEXT(vc, link);
    if(vc == v->cmc)
      continue;
    vllp_channel_net_close(v, vc, reason);
  }

  if(v->current_tx_buf) {
    pbuf_free(v->current_tx_buf);
    v->current_tx_buf = NULL;
  }
  v->current_tx_len = 0;
  v->current_tx_channel = 0;

  v->connected = 0;
}


static pbuf_t *
vllp_accept_syn(vllp_t *v, const uint8_t *data, size_t len,
                pbuf_t *pb)
{
  if(len != 7)
    return pb;

  if(data[1] != VLLP_VERSION) {
    evlog(LOG_DEBUG, "VLLP: Got VLLP SYN for unsuppored version %d (expected %d)",
          data[1], VLLP_VERSION);
    return pb;
  }

  if(v->connected) {
    vllp_disconnect(v, "reconnected");
  }

  v->connected = 1;
  v->SE = VLLP_HDR_E;
  memcpy(&v->crc_IV, data + 3, sizeof(v->crc_IV));

  v->channel_iv_cnt = 0;
  uint32_t cmc_iv = vllp_gen_channel_crc(v);
  v->cmc->tx_crc_IV = cmc_iv;
  v->cmc->rx_crc_IV = ~cmc_iv;

  return vllp_tx_ack(v, pb);
}


static inline void
send_cmc_message(vllp_t *v, vllp_channel_t *cmc, pbuf_t *pb,
                 int opcode, int target_channel, error_t err)
{
  pbuf_reset(pb, 1, 0);
  uint8_t *u8 = pbuf_append(pb, 3);
  u8[0] = (opcode << 4) | target_channel;
  u8[1] = err;
  u8[2] = err >> 8;
  STAILQ_INSERT_TAIL(&cmc->txq, pb, pb_link);
}


static void
vllp_net_event_cb(void *opaque, uint32_t events)
{
  vllp_channel_t *vc = opaque;
  net_task_raise(&vc->task, events);
}

static const pushpull_net_fn_t vllp_net_fn = {
  .event = vllp_net_event_cb,
};


static error_t
handle_cmc_open(vllp_t *v, vllp_channel_t *cmc,
                int target_channel,
                const void *name, size_t namelen)
{
  const uint32_t iv = vllp_gen_channel_crc(v);

  const service_t *s = service_find_by_namelen(name, namelen);
  error_t err = 0;
  if(s == NULL) {
    evlog(LOG_WARNING, "VLLP: Service %.*s does not exist",
          (int)namelen, (const char *)name);
    return ERR_NOT_FOUND;
  }

  if(vllp_channel_find(v, target_channel))
    return ERR_EXIST; // Peer is confused, channel already open

  vllp_channel_t *vc = vllp_channel_make(v, target_channel);
  if(vc == NULL)
    return ERR_NO_MEMORY;

  vc->tx_crc_IV = iv;
  vc->rx_crc_IV = ~iv;

  vc->state = VLLP_CHANNEL_STATE_ESTABLISHED;

  vc->pp.max_fragment_size = PBUF_DATA_SIZE;
  vc->pp.preferred_offset = 0;
  vc->pp.net = &vllp_net_fn;
  vc->pp.net_opaque = vc;

  err = service_open_pushpull(s, &vc->pp);
  if(err) {
    LIST_REMOVE(vc, link);
    evlog(LOG_DEBUG, "VLLP: failed to open service %s on channel %d -- %s",
          s->name, vc->id, error_to_string(err));
    free(vc);
    return err;
  }

  TAILQ_INSERT_TAIL(&v->established_channels, vc, qlink);
  evlog(LOG_DEBUG, "VLLP: service open %s on channel %d", s->name,
        vc->id);
  return 0;
}


static inline error_t
handle_cmc_close(vllp_t *v, vllp_channel_t *cmc, pbuf_t *pb,
                 int target_channel, const uint8_t *data, size_t len)
{
  error_t err = 0;
  vllp_channel_t *vc = vllp_channel_find(v, target_channel);
  if(vc != NULL) {

    if(vc != cmc && len == 2) {

      int16_t error_code = data[0] | (data[1] << 8);
      vllp_channel_net_close(v, vc, error_to_string(error_code));

    } else {
      err = ERR_MALFORMED;
    }
  }

  pbuf_free(pb);
  return err;
}

static error_t
handle_cmc(vllp_t *v, vllp_channel_t *cmc, pbuf_t *pb)
{
  size_t len = pb->pb_buflen;
  const uint8_t *u8 = pbuf_cdata(pb, 0);
  error_t err;

  if(len < 1) {
    pbuf_free(pb);
    return ERR_BAD_STATE;
  }

  uint8_t opcode = u8[0] >> 4;
  uint8_t target_channel = u8[0] & 0xf;

  switch(opcode) {
  case VLLP_CMC_OPCODE_OPEN:
    err = handle_cmc_open(v, cmc, target_channel, u8 + 1, len - 1);
    send_cmc_message(v, cmc, pb, VLLP_CMC_OPCODE_OPEN_RESPONSE,
                     target_channel, err);
    return 0;
  case VLLP_CMC_OPCODE_CLOSE:
    return handle_cmc_close(v, cmc, pb, target_channel, u8 + 1, len - 1);

  default:
    return ERR_BAD_STATE;
  }
}


error_t
vllp_channel_receive(vllp_t *v, int channel_id,
                     const uint8_t *data, size_t len)
{
  int last = data[0] & VLLP_HDR_L;
  data++;
  len--;

  vllp_channel_t *vc = vllp_channel_find(v, channel_id);
  if(vc == NULL) {
    // We just ignore these errors
    return 0;
  }

  if(vc->net_closed)
    return 0;

  if(last && !((1 << channel_id) & v->local_flow_status)) {
    return ERR_NO_BUFFER;
  }

  const int fragment_len = len;
  pbuf_t *pb = STAILQ_LAST(&vc->rxq, pbuf, pb_link);
  if(pb != NULL) {
    pbuf_t *next = NULL;
    // Squeeze in as much data as possible at end
    size_t avail = PBUF_DATA_SIZE - pb->pb_buflen;
    size_t to_copy = MIN(avail, len);

    if(to_copy > len) {
      // Can't fit all, if we need to alloc.
      next = pbuf_make(0, 0);
      if(next == NULL) {
        // No buffers, bail out
        return ERR_NO_BUFFER;
      }
      next->pb_flags = 0;
    }

    memcpy(pb->pb_data + pb->pb_buflen, data, to_copy);
    pb->pb_buflen += to_copy;
    data += to_copy;
    len -= to_copy;
    pb = next;
  } else {
    pb = pbuf_make(0, 0);
    if(pb == NULL)
      return ERR_NO_BUFFER;
    pb->pb_flags = 0;
  }

  if(pb != NULL) {

    assert(len <= PBUF_DATA_SIZE);
    STAILQ_INSERT_TAIL(&vc->rxq, pb, pb_link);

    memcpy(pb->pb_data, data, len);
    pb->pb_buflen += len;
  }

  pb = STAILQ_FIRST(&vc->rxq);
  pb->pb_pktlen += fragment_len;

  if(!last)
    return 0;

  pb = STAILQ_LAST(&vc->rxq, pbuf, pb_link);
  pb->pb_flags = PBUF_EOP;

  pb = STAILQ_FIRST(&vc->rxq);
  pb->pb_flags |= PBUF_SOP;
  STAILQ_INIT(&vc->rxq);

  if(calc_crc32(pb, vc->rx_crc_IV)) {
    return ERR_CHECKSUM_ERROR;
  }
  vc->rx_crc_IV++;
  pbuf_trim(pb, 4); // Remove CRC

  // Ownership of pb is transfered to callee
  if(vc == v->cmc) {
    return handle_cmc(v, vc, pb);
  }

  int events = vc->pp.app->push(vc->pp.app_opaque, pb);
  if(events)
    net_task_raise(&vc->task, events);
  return 0;
}


static int
fdcan_adapation_pad_ladder(int len)
{
  if(len < 12)
    return 12;
  if(len < 16)
    return 16;
  if(len < 20)
    return 20;
  if(len < 24)
    return 24;
  if(len < 32)
    return 32;
  if(len < 48)
    return 48;
  return 64;
}


static pbuf_t *
vllp_tx(vllp_t *v, pbuf_t *pb)
{
  pbuf_t *src = v->current_tx_buf;
  if(src == NULL)
    return pb;

  if(pb == NULL) {
    pb = pbuf_make(4, 0); /* offset: 4 bytes for ID prefix in dsig.c */
  } else {
    pbuf_reset(pb, 4, 0);
  }

  if(pb != NULL) {

    pb->pb_pktlen = pb->pb_buflen = v->current_tx_len + 1;
    int last = src->pb_pktlen == v->current_tx_len ? VLLP_HDR_L : 0;

    if(pbuf_pullup(src, v->current_tx_len)) {
      panic("vllp_tx");
    }

    memcpy(pbuf_data(pb, 1), pbuf_cdata(src, 0), v->current_tx_len);

    if(pb->pb_buflen > 8) {
      int len = fdcan_adapation_pad_ladder(pb->pb_buflen);
      int pad = len - pb->pb_buflen;
      uint8_t *padding = pbuf_append(pb, pad);
      padding[pad - 1] = pad;
    }

    uint8_t *hdr = pbuf_data(pb, 0);

    int channel = v->current_tx_channel;
    int flow = (1 << channel) & v->local_flow_status ? VLLP_HDR_F : 0;

    v->transmitted_local_flow_status =
      (v->transmitted_local_flow_status & ~(1 << channel)) |
      (flow ? 1 << channel : 0);

    hdr[0] = v->SE | last | flow | channel;

    dsig_emit_pbuf(v->txid, pb);
  } else {
    // Tx-Drop - no bufs
  }

  // If we fail to allocate a packet, also arm timers as this is
  // equivivalent to a packet loss
  net_timer_arm(&v->rtx_timer, clock_get() + 25000);
  net_timer_arm(&v->ack_timer, clock_get() + 1000000);
  return NULL;
}


static pbuf_t *
vllp_fragment(vllp_t *v, pbuf_t *pb)
{
  size_t payload_mtu = v->mtu - 1;
  v->current_tx_len = MIN(payload_mtu, v->current_tx_buf->pb_pktlen);
  v->SE ^= VLLP_HDR_S;
  return vllp_tx(v, pb);
}


static pbuf_t *
vllp_channel_tx(vllp_t *v, vllp_channel_t *vc, pbuf_t *pb, pbuf_t *reuse)
{
  uint32_t crc32 = calc_crc32(pb, vc->tx_crc_IV);
  vc->tx_crc_IV++;
  // Fix this for chained PBUFs, pbuf_append() will assert for now tho
  uint8_t *crcbuf = pbuf_append(pb, 4);
  crcbuf[0] = crc32;
  crcbuf[1] = crc32 >> 8;
  crcbuf[2] = crc32 >> 16;
  crcbuf[3] = crc32 >> 24;

  v->current_tx_buf = pb;
  v->current_tx_channel = vc->id;

  reuse = vllp_fragment(v, reuse);

  // Move to tail for round-robin scheduling
  TAILQ_REMOVE(&v->established_channels, vc, qlink);
  TAILQ_INSERT_TAIL(&v->established_channels, vc, qlink);
  return reuse;
}


static error_t
vllp_tx_close(vllp_t *v, vllp_channel_t *vc)
{
  TAILQ_REMOVE(&v->established_channels, vc, qlink);
  vc->state = VLLP_CHANNEL_STATE_CLOSED_SENT;

  if(!v->connected)
    return 0;

  pbuf_t *pb = pbuf_make(0, 0);
  if(pb == NULL)
    return ERR_NO_BUFFER;

  send_cmc_message(v, v->cmc, pb, VLLP_CMC_OPCODE_CLOSE, vc->id, 0);
  pb = pbuf_splice(&v->cmc->txq);
  vllp_channel_tx(v, v->cmc, pb, NULL);
  return 0;
}


static pbuf_t *
vllp_maybe_tx(vllp_t *v, pbuf_t *reuse)
{
  if(v->current_tx_buf)
    return reuse;

  vllp_channel_t *vc;
  TAILQ_FOREACH(vc, &v->established_channels, qlink) {
    pbuf_t *out;
    if(vc->pp.app != NULL) {

      if(vc->app_closed == 1) {
        if(vllp_tx_close(v, vc))
          continue; // Close failed (no buffers), retry later

        // Ok we sent something
        vc->app_closed = 2;
        vllp_channel_maybe_destroy(v, vc);
        return reuse;

      } else {

        if(vc->net_closed)
          continue;

        out = vc->pp.app->pull(vc->pp.app_opaque);
      }

    } else {
      out = pbuf_splice(&vc->txq);
    }

    if(out == NULL)
      continue;
    return vllp_channel_tx(v, vc, out, reuse);
  }

  if(v->transmitted_local_flow_status != v->local_flow_status) {
    return vllp_tx_ack(v, reuse);
  }
  return reuse;
}


static pbuf_t *
vllp_ack_payload(vllp_t *v, pbuf_t *pb)
{
  timer_disarm(&v->rtx_timer);

  v->current_tx_buf = pbuf_drop(v->current_tx_buf, v->current_tx_len, 1);
  if(v->current_tx_buf) {
    pb = vllp_fragment(v, pb);
  }
  return pb;
}


static pbuf_t *
vllp_rx(vllp_t *v, pbuf_t *pb)
{
  net_timer_arm(&v->timeout_timer, clock_get() + v->timeout * 1000000);
  vllp_refresh_local_flow_status(v);

  size_t len = pb ? pb->pb_buflen : 0;
  const uint8_t *u8 = pb ? pbuf_cdata(pb, 0) : NULL;

  if(len < 1)
    return pb;

  if(len > 8) {
    int pad = u8[len - 1];
    if(pad >= len) {
      vllp_disconnect(v, "invalid pad");
      return pb;
    }
    len -= pad;
  }

  uint8_t hdr = u8[0];
  if(hdr == VLLP_SYN) {
    return vllp_accept_syn(v, u8, len, pb);
  }

  if((u8[0] & 0x1f) == 0x1f) {
    // ACK packet
    if(~crc32(v->crc_IV, u8, len)) {
      return pb;
    }

    if(len != 7)
      return pb;

    v->remote_flow_status = u8[1] | (u8[2] << 8);
  }

  const int peer_accepted =
    !(u8[0] & VLLP_HDR_E) != !(v->SE & VLLP_HDR_S);

  const int we_can_accept =
    !(u8[0] & VLLP_HDR_S) == !(v->SE & VLLP_HDR_E);

  if(!v->connected) {
    return pb;
  }

  int channel_id = u8[0] & 0xf;


  if(channel_id != 0xf) {

    int ack_delay = 1000;

    if(we_can_accept) {

      error_t err = vllp_channel_receive(v, channel_id, u8, len);

      switch(err) {
      case 0:
        v->transmitted_local_flow_status &= ~(1 << channel_id);
        v->SE ^= VLLP_HDR_E;
        break;
      case ERR_NO_BUFFER:
        ack_delay = 10000; // Ease off a bit as we're low on bufs
        break;
      case ERR_CHECKSUM_ERROR:
        vllp_disconnect(v, "Invalid CRC");
        return pb;
      case ERR_BAD_STATE:
        vllp_disconnect(v, "Bad state");
        return pb;
      default:
        panic("vllp_channel_receive");
      }
    }

    v->remote_flow_status = (v->remote_flow_status & ~(1 << channel_id)) |
      (u8[0] & VLLP_HDR_F ? (1 << channel_id) : 0);

    net_timer_arm(&v->ack_timer, clock_get() + ack_delay);
  }

  if(v->current_tx_buf != NULL) {

    if(!peer_accepted) {
      net_timer_arm(&v->rtx_timer, clock_get() + 1000);
    } else {
      pb = vllp_ack_payload(v, pb);
    }
  }

  return vllp_maybe_tx(v, pb);
}


static void
vllp_ack_timer(void *opaque, uint64_t expire)
{
  vllp_t *v = opaque;
  vllp_refresh_local_flow_status(v);
  vllp_tx_ack(v, NULL);
}

static void
vllp_rtx_timer(void *opaque, uint64_t expire)
{
  vllp_t *v = opaque;
  vllp_refresh_local_flow_status(v);
  vllp_tx(v, NULL);
}


static void
vllp_channel_task_cb(net_task_t *nt, uint32_t signals)
{
  vllp_channel_t *vc = ((void *)nt) - offsetof(vllp_channel_t, task);
  vllp_t *v = vc->vllp;

  if(signals & PUSHPULL_EVENT_CLOSE) {
    if(vc->app_closed)
      return;

    vc->app_closed = 1;
  }

  if(vllp_refresh_local_flow_status(v)) {
    vllp_tx_ack(v, NULL);
  }

  vllp_maybe_tx(v, NULL);
}


pbuf_t *
vllp_input(uint32_t id, pbuf_t *pb)
{
  vllp_t *v;
  LIST_FOREACH(v, &vllps, link) {
    if(v->rxid == id) {
      pb = vllp_rx(v, pb);
      if(pb != NULL) {
        // Passed buffer was not recycled, free it
        pbuf_free(pb);
      }
      // Return NULL means we handled the packet
      return NULL;
    }
  }
  // No match, pass on to other dsig subscribers, tec
  return pb;
}

static void
vllp_timeout_timer(void *opaque, uint64_t now)
{
  vllp_t *v = opaque;
  vllp_disconnect(v, "timeout");
}


vllp_t *
vllp_server_create(uint32_t txid, uint32_t rxid, uint8_t mtu,
                   uint8_t timeout)
{
  vllp_t *v = xalloc(sizeof(vllp_t), 0, MEM_MAY_FAIL | MEM_CLEAR);
  if(v == NULL)
    return NULL;

  v->cmc = vllp_channel_make(v, 14);
  if(v->cmc == NULL) {
    free(v);
    return NULL;
  }
  v->cmc->state = VLLP_CHANNEL_STATE_ESTABLISHED;

  TAILQ_INIT(&v->established_channels);
  TAILQ_INSERT_TAIL(&v->established_channels, v->cmc, qlink);

  v->rxid = rxid;
  v->txid = txid;
  if(mtu > 8)
    mtu--; // For FDCAN adaptation

  v->mtu = mtu;
  v->timeout = timeout;
  v->rtx_timer.t_cb = vllp_rtx_timer;
  v->rtx_timer.t_opaque = v;
  v->rtx_timer.t_name = "vllprtx";

  v->ack_timer.t_cb = vllp_ack_timer;
  v->ack_timer.t_opaque = v;
  v->ack_timer.t_name = "vllprtx";

  v->timeout_timer.t_cb = vllp_timeout_timer;
  v->timeout_timer.t_opaque = v;
  v->timeout_timer.t_name = "vllptimout";

  LIST_INSERT_HEAD(&vllps, v, link);
  return v;
}

static const char vllp_channel_state_strtbl[] = {
  "PENDING\0"
  "OPEN_SENT\0"
  "ESTABLISHED\0"
  "CLOSED_SEND\0"
  "\0"
};

static const char vllp_channel_app_closed_strtbl[] = {
  "OPEN\0"
  "APP_CLOSED\0"
  "CLOSED_SENT\0"
  "\0"
};

static const char vllp_channel_net_closed_strtbl[] = {
  "OPEN\0"
  "CLOSED\0"
  "\0"
};

static error_t
cmd_tcp(cli_t *cli, int argc, char **argv)
{
  vllp_t *v;
  vllp_channel_t *vc;
  LIST_FOREACH(v, &vllps, link) {
    cli_printf(cli, "TX:0x%x  RX:0x%x %sonnected", v->txid, v->rxid,
               v->connected ? "C" : "Disc");
    cli_printf(cli, "  Flow status Local:0x%04x Remote:0x%04x\n",
               v->local_flow_status, v->remote_flow_status);
    cli_printf(cli, "  Channels:\n");
    LIST_FOREACH(vc, &v->channels, link) {
      cli_printf(cli, "    %2d : state:%s app:%s net:%s\n", vc->id,
                 strtbl(vllp_channel_state_strtbl, vc->state),
                 strtbl(vllp_channel_app_closed_strtbl, vc->app_closed),
                 strtbl(vllp_channel_net_closed_strtbl, vc->net_closed));
    }

    cli_printf(cli, "\n");
  }
  return 0;
}

CLI_CMD_DEF_EXT("show_vllp", cmd_tcp, NULL, "Show VLLP connections");
