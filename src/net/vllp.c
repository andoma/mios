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

#include <sys/param.h>

#include "util/crc32.h"
#include "net/pbuf.h"
#include "net/net_task.h"

#include "irq.h"

LIST_HEAD(vllp_channel_list, vllp_channel);
TAILQ_HEAD(vllp_channel_queue, vllp_channel);


typedef struct vllp {

  struct vllp_channel_list channels;
  struct vllp_channel_queue established_channels;

  timer_t ack_timer;
  timer_t rtx_timer;

  pbuf_t *current_tx_buf;
  uint16_t current_tx_len;
  uint8_t current_tx_channel;

  struct vllp_channel *cmc;

  uint32_t txid;
  uint32_t crc_IV;

  uint16_t remote_flow_status;
  uint16_t local_flow_status;

  uint8_t connected;
  uint8_t SE;
  uint8_t mtu;

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

  uint8_t flags;
  uint8_t id;
  uint8_t state;
  uint8_t app_closed;
  uint8_t net_closed;
};


#define VLLP_VERSION 1

#define VLLP_SYN   0x0f

#define VLLP_HDR_S 0x80
#define VLLP_HDR_E 0x40
#define VLLP_HDR_F 0x20
#define VLLP_HDR_L 0x10

#define VLLP_CMC_OPCODE_OPEN_WITH_CRC32   0
#define VLLP_CMC_OPCODE_OPEN_NO_CRC32     1
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
vllp_append_crc(vllp_t *v, uint8_t *pkt, size_t len)
{
  uint32_t crc = ~crc32(v->crc_IV, pkt, len);
  pkt[len + 0] = crc;
  pkt[len + 1] = crc >> 8;
  pkt[len + 2] = crc >> 16;
  pkt[len + 3] = crc >> 24;
}


static void
vllp_channel_destroy(vllp_t *v, vllp_channel_t *vc)
{
  LIST_REMOVE(vc, link);

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

    if(!vc->pp.app->may_push || !vc->pp.app->may_push(vc->pp.app_opaque))
      bits &= ~(1 << vc->id);
  }

  if(v->local_flow_status == bits)
    return 0;
  v->local_flow_status = bits;
  return 1;
}


static void
vllp_tx_ack(vllp_t *v)
{
  uint8_t pkt[7];
  pkt[0] = v->SE | 0x1f;
  pkt[1] = v->local_flow_status;
  pkt[2] = v->local_flow_status >> 8;
  vllp_append_crc(v, pkt, 3);
  dsig_emit(v->txid, pkt, sizeof(pkt));

  net_timer_arm(&v->ack_timer, clock_get() + 1000000);
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
vllp_disconnect(vllp_t *v, const char *reason)
{
  vllp_channel_t *vc, *n;

  if(!v->connected)
    return;

  evlog(LOG_DEBUG, "VLLP: Disconnected -- %s", reason);

  for(vc = LIST_FIRST(&v->channels); vc != NULL; vc = n) {
    n = LIST_NEXT(vc, link);
    if(vc == v->cmc)
      continue;

    if(!vc->net_closed) {
      vc->pp.app->close(vc->pp.app_opaque, reason);
      vc->net_closed = 1;
    }
    vllp_channel_maybe_destroy(v, vc);
  }

  if(v->current_tx_buf) {
    pbuf_free(v->current_tx_buf);
    v->current_tx_buf = 0;
  }
  v->current_tx_len = 0;
  v->current_tx_channel = 0;

  v->connected = 0;
}


static void
vllp_accept_syn(vllp_t *v, const uint8_t *data, size_t len)
{
  if(len != 7)
    return;

  if(v->connected) {
    vllp_disconnect(v, "reconnected");
  }

  v->connected = 1;
  v->SE = VLLP_HDR_E;
  memcpy(&v->crc_IV, data + 3, sizeof(v->crc_IV));
  vllp_tx_ack(v);
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
                int opcode, int target_channel,
                const void *name, size_t namelen)
{
  const service_t *s = service_find_by_namelen(name, namelen);
  error_t err = 0;
  if(s == NULL)
    return ERR_NOT_FOUND;

  if(vllp_channel_find(v, target_channel))
    return ERR_EXIST; // Peer is confused, channel already open

  vllp_channel_t *vc = vllp_channel_make(v, target_channel);
  if(vc == NULL)
    return ERR_NO_MEMORY;

  vc->state = VLLP_CHANNEL_STATE_ESTABLISHED;

  vc->pp.max_fragment_size = v->mtu - 1 - 4;
  vc->pp.preferred_offset = 0;
  vc->pp.net = &vllp_net_fn;
  vc->pp.net_opaque = vc;

  err = service_open_pushpull(s, &vc->pp);
  if(err) {
    LIST_REMOVE(vc, link);
    free(vc);
    evlog(LOG_DEBUG, "VLLP: failed to open server %s -- %s",
          s->name, error_to_string(err));
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

      if(!vc->net_closed) {
        vc->pp.app->close(vc->pp.app_opaque, error_to_string(error_code));
        vc->net_closed = 1;
      }
      vllp_channel_maybe_destroy(v, vc);

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
  case VLLP_CMC_OPCODE_OPEN_WITH_CRC32:
  case VLLP_CMC_OPCODE_OPEN_NO_CRC32:
    err = handle_cmc_open(v, cmc, opcode, target_channel,
                          u8 + 1, len - 1);
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

  if(calc_crc32(pb, v->crc_IV)) {
    return ERR_CHECKSUM_ERROR;
  }

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
  if(len < 11)
    return 12;
  if(len < 15)
    return 16;
  if(len < 19)
    return 20;
  if(len < 23)
    return 24;
  if(len < 31)
    return 32;
  if(len < 47)
    return 48;
  return 64;
}


static void
vllp_tx(vllp_t *v)
{
  const pbuf_t *src = v->current_tx_buf;
  if(src == NULL)
    return;

  pbuf_t *pb = pbuf_make(5, 0); /* offset:
                                   1 byte for VLLP header
                                   4 bytes for ID prefix in dsig.c */
  if(pb != NULL) {

    pb->pb_pktlen = pb->pb_buflen = v->current_tx_len + 1;
    int last = src->pb_pktlen == v->current_tx_len ? VLLP_HDR_L : 0;

    memcpy(pbuf_data(pb, 1), pbuf_cdata(src, 0), pb->pb_pktlen);

    if(pb->pb_buflen > 8) {
      int len = fdcan_adapation_pad_ladder(pb->pb_buflen);
      int pad = len - pb->pb_buflen;
      uint8_t *padding = pbuf_append(pb, pad);
      padding[pad - 1] = pad;
    }

    uint8_t *hdr = pbuf_data(pb, 0);

    int channel = v->current_tx_channel;
    int flow = (1 << channel) & v->local_flow_status ? VLLP_HDR_F : 0;

    hdr[0] = v->SE | last | flow | channel;

    dsig_emit_pbuf(v->txid, pb);
  }

  // If we fail to allocate a packet, also arm timers as this is
  // equivivalent to a packet loss
  net_timer_arm(&v->rtx_timer, clock_get() + 25000);
  net_timer_arm(&v->ack_timer, clock_get() + 1000000);
}


static void
vllp_fragment(vllp_t *v)
{
  size_t payload_mtu = v->mtu - 1;
  v->current_tx_len = MIN(payload_mtu, v->current_tx_buf->pb_pktlen);
  v->SE ^= VLLP_HDR_S;
  vllp_tx(v);
}


static void
vllp_channel_tx(vllp_t *v, vllp_channel_t *vc, pbuf_t *pb)
{
  if(1) { // TODO: Make optional depending on channel type
    uint32_t crc32 = calc_crc32(pb, v->crc_IV);
    // Fix this for chained PBUFs, pbuf_append() will assert for now tho
    uint8_t *crcbuf = pbuf_append(pb, 4);
    crcbuf[0] = crc32;
    crcbuf[1] = crc32 >> 8;
    crcbuf[2] = crc32 >> 16;
    crcbuf[3] = crc32 >> 24;
  }

  v->current_tx_buf = pb;
  v->current_tx_channel = vc->id;

  vllp_fragment(v);

  // Move to tail for round-robin scheduling
  TAILQ_REMOVE(&v->established_channels, vc, qlink);
  TAILQ_INSERT_TAIL(&v->established_channels, vc, qlink);
}


static error_t
vllp_tx_close(vllp_t *v, vllp_channel_t *vc)
{
  pbuf_t *pb = pbuf_make(0, 0);
  if(pb == NULL)
    return ERR_NO_BUFFER;

  send_cmc_message(v, v->cmc, pb, VLLP_CMC_OPCODE_CLOSE, vc->id, 0);

  TAILQ_REMOVE(&v->established_channels, vc, qlink);
  vc->state = VLLP_CHANNEL_STATE_CLOSED_SENT;
  pb = pbuf_splice(&v->cmc->txq);
  vllp_channel_tx(v, v->cmc, pb);
  return 0;
}


static void
vllp_maybe_tx(vllp_t *v)
{
  if(v->current_tx_buf)
    return;

  vllp_channel_t *vc;
  TAILQ_FOREACH(vc, &v->established_channels, qlink) {
    pbuf_t *pb;
    if(vc->pp.app != NULL) {

      if(vc->app_closed == 1) {
        if(vllp_tx_close(v, vc))
          continue; // Close failed (no buffers), retry later

        // Ok we sent something
        vc->app_closed = 2;
        vllp_channel_maybe_destroy(v, vc);
        return;

      } else {
        pb = vc->pp.app->pull(vc->pp.app_opaque);
      }

    } else {
      pb = pbuf_splice(&vc->txq);
    }

    if(pb == NULL)
      continue;
    vllp_channel_tx(v, vc, pb);
    return;
  }
}


static void
vllp_ack_payload(vllp_t *v)
{
  timer_disarm(&v->rtx_timer);

  v->current_tx_buf = pbuf_drop(v->current_tx_buf, v->current_tx_len);

  if(v->current_tx_buf != NULL) {
    // More data in message
    vllp_fragment(v);
  }
}


static void
vllp_dsig_rx(void *opaque, const struct pbuf *pb, uint32_t signal)
{
  vllp_t *v = opaque;

  vllp_refresh_local_flow_status(v);

  size_t len = pb ? pb->pb_buflen : 0;
  const uint8_t *u8 = pb ? pbuf_cdata(pb, 0) : NULL;

  if(u8 == NULL) {
    // timeout
    vllp_disconnect(v, "timeout");
    return;
  }

  if(len < 1)
    return;

  if(len > 8) {
    int pad = u8[len - 1];
    if(pad >= len) {
      vllp_disconnect(v, "invalid pad");
      return;
    }
    len -= pad;
  }

  uint8_t hdr = u8[0];
  if(hdr == VLLP_SYN) {
    return vllp_accept_syn(v, u8, len);
  }

  if((u8[0] & 0x1f) == 0x1f) {
    // ACK packet
    if(~crc32(v->crc_IV, u8, len)) {
      return;
    }

    if(len != 7)
      return;

    v->remote_flow_status = u8[1] | (u8[2] << 8);
  }

  const int peer_accepted =
    !(u8[0] & VLLP_HDR_E) != !(v->SE & VLLP_HDR_S);

  const int we_can_accept =
    !(u8[0] & VLLP_HDR_S) == !(v->SE & VLLP_HDR_E);

  if(!v->connected) {
    return;
  }

  int channel_id = u8[0] & 0xf;


  if(channel_id != 0xf) {

    int ack_delay = 1000;

    if(we_can_accept) {

      error_t err = vllp_channel_receive(v, channel_id, u8, len);

      switch(err) {
      case 0:
        v->SE ^= VLLP_HDR_E;
        break;
      case ERR_NO_BUFFER:
        ack_delay = 10000; // Ease off a bit as we're low on bufs
        break;
      case ERR_CHECKSUM_ERROR:
        vllp_disconnect(v, "Invalid CRC");
        return;
      case ERR_BAD_STATE:
        vllp_disconnect(v, "Bad state");
        return;
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
      vllp_ack_payload(v);
    }
  }

  vllp_maybe_tx(v);
}


static void
vllp_ack_timer(void *opaque, uint64_t expire)
{
  vllp_t *v = opaque;
  vllp_refresh_local_flow_status(v);
  vllp_tx_ack(v);
}

static void
vllp_rtx_timer(void *opaque, uint64_t expire)
{
  vllp_t *v = opaque;
  vllp_refresh_local_flow_status(v);
  vllp_tx(v);
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
    vllp_tx_ack(v);
  }

  vllp_maybe_tx(v);
}


vllp_t *
vllp_server_create(uint32_t txid, uint32_t rxid, uint8_t mtu)
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

  v->txid = txid;
  if(mtu > 8)
    mtu--; // For FDCAN adaptation

  v->mtu = mtu;

  v->rtx_timer.t_cb = vllp_rtx_timer;
  v->rtx_timer.t_opaque = v;
  v->rtx_timer.t_name = "vllprtx";

  v->ack_timer.t_cb = vllp_ack_timer;
  v->ack_timer.t_opaque = v;
  v->ack_timer.t_name = "vllprtx";

  dsig_sub(rxid, -1, 3000, vllp_dsig_rx, v);
  return v;
}
