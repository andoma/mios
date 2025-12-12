#define _GNU_SOURCE
#include "vllp.h"

#include <assert.h>
#include <sys/queue.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <syslog.h>
#include <stdarg.h>

#include <sys/random.h>
#include <sys/param.h>

#define VLLP_ACK_INTERVAL 1000000
#define VLLP_RTX_TIMEOUT  25000


extern void pts();

LIST_HEAD(vllp_channel_list, vllp_channel);
TAILQ_HEAD(vllp_channel_queue, vllp_channel);
TAILQ_HEAD(vllp_pkt_queue, vllp_pkt);

typedef struct vllp_pkt {
  TAILQ_ENTRY(vllp_pkt) link;
  size_t len;
  uint8_t type;
  uint8_t data[0];
} vllp_pkt_t;

#define VLLP_PKT_MSG 0
#define VLLP_PKT_EOF 1
#define VLLP_PKT_RDY 2

struct vllp {

  struct vllp_pkt_queue rxq;

  struct vllp_pkt *current_tx;

  int64_t next_ack;
  int64_t next_rtx;

  void (*tx)(void *opaque, const void *data, size_t len);
  open_channel_result_t (*open_channel)(void *opaque, const char *name,
                                        vllp_channel_t *vc);
  void (*log)(void *opaque, int level, const char *msg);

  void *opaque;

  struct vllp_channel *cmc;
  struct vllp_channel_list channels;
  struct vllp_channel_queue pending_open;
  struct vllp_channel_queue active_channels;

  pthread_t tid;

  pthread_mutex_t mutex;
  pthread_cond_t cond;

  uint32_t flags;

  uint32_t crc_IV;
  uint32_t channel_iv_cnt;

  uint16_t available_channel_ids;
  uint16_t remote_flow_status;
  uint16_t local_flow_status;

  uint8_t run;
  uint8_t connected;
  uint8_t SE;
  uint8_t mtu;
  uint8_t timeout;

  uint32_t refcount;
};

static inline int
is_client(vllp_t *v)
{
  return v->open_channel == NULL;
}

static inline int
is_server(vllp_t *v)
{
  return v->open_channel != NULL;
}

#define VLLP_VERSION 2

#define VLLP_SYN   0x0f

#define VLLP_HDR_S 0x80
#define VLLP_HDR_E 0x40
#define VLLP_HDR_F 0x20
#define VLLP_HDR_L 0x10

#define VLLP_CMC_OPCODE_OPEN              0
#define VLLP_CMC_OPCODE_OPEN_RESPONSE     2
#define VLLP_CMC_OPCODE_CLOSE             3

typedef enum {
  VLLP_CHANNEL_STATE_CREATED,
  VLLP_CHANNEL_STATE_PENDING_OPEN,
  VLLP_CHANNEL_STATE_OPEN_SENT,
  VLLP_CHANNEL_STATE_ESTABLISHED,
  VLLP_CHANNEL_STATE_ACTIVE,
  VLLP_CHANNEL_STATE_CLOSE_SENT,
  VLLP_CHANNEL_STATE_CLOSED,
} vllp_channel_state_t;


struct vllp_channel {
  vllp_t *vllp;

  LIST_ENTRY(vllp_channel) link;
  TAILQ_ENTRY(vllp_channel) qlink;

  struct vllp_pkt_queue txq;
  struct vllp_pkt_queue mtxq;
  struct vllp_pkt_queue rxq;

  pthread_cond_t state_cond;
  pthread_cond_t rxq_cond;

  pthread_t rx_thread;
  void (*rx)(void *opaque, const void *data, size_t length);
  void (*eof)(void *opaque, int error_code);
  void (*rdy)(void *opaque);
  void *opaque;

  uint32_t tx_crc_IV;
  uint32_t rx_crc_IV;

  char *rxbuf;
  size_t rxlen;
  size_t rxcap;

  char *name;

  uint32_t flags;
  int refcount;
  vllp_channel_state_t state;
  uint8_t id;
  uint8_t rx_thread_run;
  uint8_t is_closed;
  uint32_t closed_status;
};

static int cmc_rx(void *opaque, const void *data, size_t len);

static void vllp_retain(vllp_t *v);

static void vllp_release(vllp_t *v);

static int64_t
get_ts(void)
{
  struct timespec tv;
#ifdef __linux__
  clock_gettime(CLOCK_MONOTONIC, &tv);
#else
  clock_gettime(CLOCK_REALTIME, &tv);
#endif
  return (int64_t)tv.tv_sec * 1000000LL + (tv.tv_nsec / 1000);
}



static void __attribute__((unused))
hexdump(const char *pfx, const void *data_, int len)
{
  int i, j, k;
  const uint8_t *data = data_;
  char buf[100];

  for(i = 0; i < len; i+= 16) {
    int p = snprintf(buf, sizeof(buf), "0x%06x: ", i);

    for(j = 0; j + i < len && j < 16; j++) {
      p += snprintf(buf + p, sizeof(buf) - p, "%s%02x ",
                    j==8 ? " " : "", data[i+j]);
    }
    const int cnt = (17 - j) * 3 + (j < 8);
    for(k = 0; k < cnt; k++)
      buf[p + k] = ' ';
    p += cnt;

    for(j = 0; j + i < len && j < 16; j++)
      buf[p++] = data[i+j] < 32 || data[i+j] > 126 ? '.' : data[i+j];
    buf[p] = 0;
    printf("%s: %s\n", pfx, buf);
  }
}


static void
vllp_log(vllp_t *v, int level, const char *msg)
{
  if(v->log == NULL)
    return;
  v->log(v->opaque, level, msg);
}

static void
vllp_channel_retain(vllp_channel_t *vc, const char *whom)
{
  __sync_add_and_fetch(&vc->refcount, 1);
}

static void
channel_enq_rx_meta(vllp_channel_t *vc, int error_code, int type)
{
  vllp_pkt_t *vp = malloc(sizeof(vllp_pkt_t) + sizeof(int));
  memcpy(vp->data, &error_code, sizeof(int));
  vp->len = sizeof(int);
  vp->type = type;
  TAILQ_INSERT_TAIL(&vc->rxq, vp, link);
  pthread_cond_signal(&vc->rxq_cond);
}

static void
vllp_channel_release(vllp_channel_t *vc, const char *whom)
{
  int r = __sync_add_and_fetch(&vc->refcount, -1);
  if(r)
    return;
  vllp_pkt_t *vp;
  while((vp = TAILQ_FIRST(&vc->txq)) != NULL) {
    TAILQ_REMOVE(&vc->txq, vp, link);
    free(vp);
  }
  while((vp = TAILQ_FIRST(&vc->mtxq)) != NULL) {
    TAILQ_REMOVE(&vc->mtxq, vp, link);
    free(vp);
  }

  free(vc->rxbuf);
  free(vc->name);
  vllp_release(vc->vllp);
  free(vc);
}


static void
vllp_channel_set_state(vllp_channel_t *vc, vllp_channel_state_t state)
{
  vc->state = state;
  pthread_cond_signal(&vc->state_cond);
}



static void
vllp_channel_unlink(vllp_channel_t *vc)
{
  LIST_REMOVE(vc, link);
  vllp_channel_release(vc, __FUNCTION__);
}

static void
vllp_disconnect(vllp_t *v, int error)
{
  if(!v->connected)
    return;

  struct vllp_channel_list tmp;
  LIST_INIT(&tmp);

  vllp_channel_t *vc, *n;
  for(vc = LIST_FIRST(&v->channels); vc != NULL; vc = n) {
    n = LIST_NEXT(vc, link);

    if(vc == v->cmc)
      continue;

    switch(vc->state) {
    case VLLP_CHANNEL_STATE_PENDING_OPEN:
      if(vc->flags & VLLP_CHANNEL_RECONNECT) {
        continue; // Just stay on this queue
      }
      TAILQ_REMOVE(&v->pending_open, vc, qlink);
      vllp_channel_release(vc, "disconnect-in-pending-open");
      break;

    case VLLP_CHANNEL_STATE_ACTIVE:
      TAILQ_REMOVE(&v->active_channels, vc, qlink);
      vllp_channel_release(vc, "disconnect-in-active");
      // FALLTHRU
    case VLLP_CHANNEL_STATE_ESTABLISHED:
      if(vc->flags & VLLP_CHANNEL_RECONNECT) {
        channel_enq_rx_meta(vc, 0, VLLP_PKT_EOF);
        vllp_channel_set_state(vc, VLLP_CHANNEL_STATE_PENDING_OPEN);
        TAILQ_INSERT_TAIL(&v->pending_open, vc, qlink);
        vllp_channel_retain(vc, "disconnect-need-reconnect");
        continue;
      }
      break;

    case VLLP_CHANNEL_STATE_CLOSE_SENT:
      vllp_channel_set_state(vc, VLLP_CHANNEL_STATE_CLOSED);
      break;

    default:
      break;
    }
    LIST_REMOVE(vc, link);
    LIST_INSERT_HEAD(&tmp, vc, link);

    if(is_client(v)) {
      v->available_channel_ids |= (1 << vc->id);
    }
  }

  pthread_mutex_unlock(&v->mutex);

  while((vc = LIST_FIRST(&tmp)) != NULL) {
    if(vc->eof != NULL)
      vc->eof(vc->opaque, error);
    vllp_channel_unlink(vc);
  }

  pthread_mutex_lock(&v->mutex);

  v->next_ack = get_ts() + VLLP_ACK_INTERVAL;
  v->next_rtx = INT64_MAX;
  v->connected = 0;
  v->SE = VLLP_HDR_E;
}




static void
vllp_generate_crc(uint32_t iv, uint8_t *pkt, size_t len)
{
  uint32_t crc = ~vllp_crc32(iv, pkt, len);
  pkt[len + 0] = crc;
  pkt[len + 1] = crc >> 8;
  pkt[len + 2] = crc >> 16;
  pkt[len + 3] = crc >> 24;
}

static uint32_t
vllp_gen_channel_crc(vllp_t *v)
{
  v->channel_iv_cnt++;
  return vllp_crc32(v->crc_IV, &v->channel_iv_cnt, sizeof(v->channel_iv_cnt));
}


static void
vllp_send_syn(vllp_t *v)
{
#ifdef __linux__
  if(getrandom(&v->crc_IV, sizeof(v->crc_IV), 0) != sizeof(v->crc_IV))
    return;
#else
  v->crc_IV_ = rand() ^ time(NULL);
#endif

  v->channel_iv_cnt = 0;
  uint32_t cmc_iv = vllp_gen_channel_crc(v);
  v->cmc->tx_crc_IV = ~cmc_iv;
  v->cmc->rx_crc_IV = cmc_iv;

  uint8_t pkt[3 + sizeof(v->crc_IV)];
  pkt[0] = VLLP_SYN;
  pkt[1] = VLLP_VERSION;
  pkt[2] = v->mtu;
  memcpy(pkt + 3, &v->crc_IV, sizeof(v->crc_IV));
  pthread_mutex_unlock(&v->mutex);
  v->tx(v->opaque, pkt, sizeof(pkt));
  pthread_mutex_lock(&v->mutex);
}

static void
vllp_send_ack(vllp_t *v)
{
  uint8_t pkt[7];

  pkt[0] = v->SE | 0x1f;
  pkt[1] = v->local_flow_status;
  pkt[2] = v->local_flow_status >> 8;
  vllp_generate_crc(v->crc_IV, pkt, 3);

  pthread_mutex_unlock(&v->mutex);
  v->tx(v->opaque, pkt, sizeof(pkt));
  pthread_mutex_lock(&v->mutex);
}


static int
vllp_accept_syn(vllp_t *v, const uint8_t *u8, size_t len, int64_t now)
{
  if(is_client(v)) {
    vllp_log(v, LOG_INFO, "Client got unexpected SYN");
    return VLLP_ERR_MALFORMED;
  }

  if(len != 3 + sizeof(v->crc_IV)) {
    vllp_log(v, LOG_ERR, "SYN packet invalid length");
    return VLLP_ERR_MALFORMED;
  }

  if(u8[1] != VLLP_VERSION) {
    vllp_log(v, LOG_ERR, "SYN packet unsupported version");
    return VLLP_ERR_MALFORMED;
  }

  if(u8[2] != v->mtu) {
    vllp_log(v, LOG_ERR, "SYN packet MTU mismatch");
    return VLLP_ERR_MALFORMED;
  }

  vllp_disconnect(v, VLLP_ERR_BAD_STATE);

  memcpy(&v->crc_IV, u8 + 3, sizeof(v->crc_IV));
  v->next_ack = now + VLLP_ACK_INTERVAL;
  v->next_rtx = INT64_MAX;
  v->connected = 1;

  v->channel_iv_cnt = 0;
  uint32_t cmc_iv = vllp_gen_channel_crc(v);
  v->cmc->tx_crc_IV = cmc_iv;
  v->cmc->rx_crc_IV = ~cmc_iv;

  vllp_send_ack(v);
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


static void
vllp_send_pkt(vllp_t *v, vllp_pkt_t *vp)
{
  int channel_id = vp->data[0] & 0xf;
  int flow = 0;

  if(channel_id != 15)
    flow = v->local_flow_status & (1 << channel_id) ? VLLP_HDR_F : 0;

  vp->data[0] = (vp->data[0] & (0xf | VLLP_HDR_L)) | v->SE | flow;

  pthread_mutex_unlock(&v->mutex);

  if(v->flags & VLLP_FDCAN_ADAPTATION && vp->len > 8) {

    uint8_t pkt[64] = {};
    memcpy(pkt, vp->data, vp->len);
    size_t len = fdcan_adapation_pad_ladder(vp->len);
    pkt[len - 1] = len - vp->len;
    v->tx(v->opaque, pkt, len);
  } else {
    v->tx(v->opaque, vp->data, vp->len);
  }
  pthread_mutex_lock(&v->mutex);
}


static void
fragment(vllp_t *v, vllp_channel_t *vc)
{
  vllp_pkt_t *m;
  while((m = TAILQ_FIRST(&vc->mtxq)) != NULL) {

    TAILQ_REMOVE(&vc->mtxq, m, link);
    if(m->type) {
      TAILQ_INSERT_TAIL(&vc->txq, m, link);
      continue;
    }

    size_t len = m->len;

    // 4 extra bytes have already been allocated for CRC
    vllp_generate_crc(vc->tx_crc_IV, m->data, len);
    vc->tx_crc_IV++;

    len += 4;

    const void *data = m->data;
    while(1) {
      size_t fsize = MIN(len + 1, v->mtu);
      vllp_pkt_t *f = malloc(sizeof(vllp_pkt_t) + fsize);

      TAILQ_INSERT_TAIL(&vc->txq, f, link);
      memcpy(f->data + 1, data, fsize - 1);
      f->data[0] = vc->id;
      f->len = fsize;
      f->type = 0;

      len -= fsize - 1;
      data += fsize - 1;

      if(len == 0) {
        f->data[0] |= VLLP_HDR_L;
        break;
      }
    }
    free(m);
  }
}


static vllp_channel_t *
channel_find(vllp_t *v, int channel_id)
{
  vllp_channel_t *vc;
  LIST_FOREACH(vc, &v->channels, link) {
    if(vc->id == channel_id)
      return vc;
  }
  return NULL;
}


// This can be called on any thread
static void
channel_send_vp(vllp_t *v, vllp_channel_t *vc, vllp_pkt_t *vp)
{
  TAILQ_INSERT_TAIL(&vc->mtxq, vp, link);
  if(vc->state == VLLP_CHANNEL_STATE_ESTABLISHED) {
    vllp_channel_set_state(vc, VLLP_CHANNEL_STATE_ACTIVE);
    TAILQ_INSERT_TAIL(&v->active_channels, vc, qlink);
    vllp_channel_retain(vc, __FUNCTION__);
    pthread_cond_signal(&v->cond);
  }
}


// This can be called on any thread
static void
channel_send_message(vllp_t *v, vllp_channel_t *vc,
                     const void *data, size_t len)
{
  // We always make place for CRC
  vllp_pkt_t *vp = malloc(sizeof(vllp_pkt_t) + len + 4);

  vp->type = VLLP_PKT_MSG;
  vp->len = len;
  memcpy(vp->data, data, len);
  channel_send_vp(v, vc, vp);
}


// This can be called on any thread
static void
channel_send_close(vllp_t *v, vllp_channel_t *vc, int error_code)
{
  vllp_channel_retain(vc, __FUNCTION__);

  // We always make place for CRC
  vllp_pkt_t *vp = malloc(sizeof(vllp_pkt_t) + sizeof(int));
  memcpy(vp->data, &error_code, sizeof(int));
  vp->type = VLLP_PKT_EOF;
  vp->len = sizeof(int);
  channel_send_vp(v, vc, vp);
}

static int
vllp_channel_receive(vllp_t *v, vllp_pkt_t *vp, int channel_id)
{
  vllp_channel_t *vc = channel_find(v, channel_id);
  if(vc == NULL)
    return 0;

  const size_t payload_len = vp->len - 1;
  if(vc->rxlen + payload_len > vc->rxcap) {
    vc->rxcap = vc->rxlen + payload_len;
    vc->rxbuf = realloc(vc->rxbuf, vc->rxcap);
  }

  memcpy(vc->rxbuf + vc->rxlen, vp->data + 1, payload_len);

  vc->rxlen += payload_len;

  if(!(vp->data[0] & VLLP_HDR_L))
    return 0;

  int rval = 0;
  size_t msglen = vc->rxlen;

  if(~vllp_crc32(vc->rx_crc_IV, vc->rxbuf, msglen)) {
    vllp_log(v, LOG_ERR, "message CRC mismatch");
    rval = VLLP_ERR_CHECKSUM_ERROR;
  } else {
    vc->rx_crc_IV++;
  }

  msglen -= 4; // Remove CRC from exposed payload

  vc->rxlen = 0;

  if(rval == 0) {

    if(vc == v->cmc) {
      rval = cmc_rx(v, vc->rxbuf, msglen);
    } else {

      vllp_pkt_t *vp = malloc(sizeof(vllp_pkt_t) + msglen);
      memcpy(vp->data, vc->rxbuf, msglen);
      vp->len = msglen;
      vp->type = VLLP_PKT_MSG;
      TAILQ_INSERT_TAIL(&vc->rxq, vp, link);
      pthread_cond_signal(&vc->rxq_cond);
    }
  }

  return rval;
}


static int
vllp_handle_rx(vllp_t *v, vllp_pkt_t *vp, int64_t now)
{
  const uint8_t *u8 = vp->data;
  size_t len = vp->len;
  if(len < 1) {
    vllp_log(v, LOG_ERR, "Received short packet");
    return VLLP_ERR_MALFORMED;
  }

  int channel_id = u8[0] & 0xf;

  if(u8[0] == VLLP_SYN) {
    return vllp_accept_syn(v, u8, len, now);
  }

  if((u8[0] & 0x1f) == 0x1f) {
    // ACK packet

    if(~vllp_crc32(v->crc_IV, u8, len)) {
      vllp_log(v, LOG_WARNING, "ACK CRC validation failed");
      return VLLP_ERR_CHECKSUM_ERROR;
    }

    if(len != 7) {
      vllp_log(v, LOG_ERR, "ACK packet length mismatch");
      return VLLP_ERR_MALFORMED;
    }

    v->remote_flow_status = u8[1] | (u8[2] << 8);
  }

  const int peer_accepted =
    !(u8[0] & VLLP_HDR_E) != !(v->SE & VLLP_HDR_S);

  const int we_can_accept =
    !(u8[0] & VLLP_HDR_S) == !(v->SE & VLLP_HDR_E);

  if(!v->connected) {
    if(is_client(v)) {

      if(u8[0] != 0x5f) {
        vllp_log(v, LOG_INFO, "Expected SYN response, got something else");
        return VLLP_ERR_BAD_STATE;
      }

      v->connected = 1;
      v->next_ack = 0;
    }
    return 0;
  }


  if(channel_id != 0xf) {

    if(we_can_accept) {

      int err = vllp_channel_receive(v, vp, channel_id);
      if(err)
        return err;

      v->SE ^= VLLP_HDR_E;
    }
    v->next_ack = MIN(v->next_ack, now + 1000);

    // Update flow status for this channel
    v->remote_flow_status =
      (v->remote_flow_status & ~(1 << channel_id)) |
      (u8[0] & VLLP_HDR_F ? (1 << channel_id) : 0);
  }

  if(v->current_tx != NULL) {
    // We have an outstanding packet

    if(!peer_accepted) {
      // Must retransmit
      v->next_rtx = MIN(v->next_rtx, now + 1000);

    } else {
      // Peer accepted our packet, drop our copy
      free(v->current_tx);
      v->current_tx = NULL;
      v->next_rtx = INT64_MAX;
    }
  }

  return 0;
}


static void
enqueue_channel_open(vllp_t *v, vllp_channel_t *vc)
{
  size_t namelen = strlen(vc->name);
  uint8_t pkt[1 + namelen];

  uint32_t iv = vllp_gen_channel_crc(v);
  vc->tx_crc_IV = ~iv;
  vc->rx_crc_IV = iv;

  int opcode = VLLP_CMC_OPCODE_OPEN;

  pkt[0] = (opcode << 4) | vc->id;
  memcpy(pkt + 1, vc->name, namelen);

  channel_send_message(v, v->cmc, pkt, 1 + namelen);
}

static void
handle_pending_channels(vllp_t *v, int64_t now)
{
  vllp_channel_t *vc;

  while((vc = TAILQ_FIRST(&v->pending_open)) != NULL) {
    TAILQ_REMOVE(&v->pending_open, vc, qlink);
    assert(vc->state == VLLP_CHANNEL_STATE_PENDING_OPEN);
    vllp_channel_set_state(vc, VLLP_CHANNEL_STATE_OPEN_SENT);
    enqueue_channel_open(v, vc);
    vllp_channel_release(vc, __FUNCTION__);
  }
}


static int64_t
vllp_tx(vllp_t *v, int64_t now)
{
  if(!v->connected) {

    if(is_server(v)) {
      // Server do nothing if not connected
      return INT64_MAX;
    }

    if(now >= v->next_ack) {
      vllp_send_syn(v);
      v->next_ack = now + VLLP_ACK_INTERVAL;
      return 0;
    }
    return v->next_ack;
  }

  if(v->current_tx) {

    // We have an outstanding packet
    if(now >= v->next_rtx) {
      v->next_rtx = now + VLLP_RTX_TIMEOUT;
      v->next_ack = now + VLLP_ACK_INTERVAL;
      vllp_send_pkt(v, v->current_tx);
      return 0;
    }
    return v->next_rtx;
  }


  // Try to find something to send
  vllp_channel_t *vc;
  TAILQ_FOREACH(vc, &v->active_channels, qlink) {

    assert(vc->state == VLLP_CHANNEL_STATE_ACTIVE);

    fragment(v, vc);

    vllp_pkt_t *vp = TAILQ_FIRST(&vc->txq);
    assert(vp != NULL);

    if(vp->type == VLLP_PKT_EOF) {
      uint8_t pkt[3];
      int error_code = 0;

      TAILQ_REMOVE(&vc->txq, vp, link);
      TAILQ_REMOVE(&v->active_channels, vc, qlink);
      vllp_channel_release(vc, "no-longer-active");

      if(vp->len == sizeof(int))
        memcpy(&error_code, vp->data, sizeof(int));

      pkt[0] = (VLLP_CMC_OPCODE_CLOSE << 4) | vc->id;
      pkt[1] = error_code;
      pkt[2] = error_code >> 8;
      channel_send_message(v, v->cmc, pkt, sizeof(pkt));
      vllp_channel_set_state(vc, VLLP_CHANNEL_STATE_CLOSE_SENT);
      free(vp);

      vllp_channel_release(vc, "close-sent");

      return vllp_tx(v, now);
    }

    if(!(v->remote_flow_status & (1 << vc->id))) {
      // may not send on this channel
      continue;
    }

    TAILQ_REMOVE(&vc->txq, vp, link);
    TAILQ_REMOVE(&v->active_channels, vc, qlink);

    v->remote_flow_status &= ~(1 << vc->id);
    v->current_tx = vp;
    v->SE ^= VLLP_HDR_S;
    v->next_rtx = now + VLLP_RTX_TIMEOUT;
    v->next_ack = now + VLLP_ACK_INTERVAL;

    if(TAILQ_FIRST(&vc->txq) == NULL) {
      // Empty, move back to established state
      vllp_channel_set_state(vc, VLLP_CHANNEL_STATE_ESTABLISHED);
      vllp_channel_release(vc, "no-longer-active");
    } else {
      // Still things to send, insert at tail
      TAILQ_INSERT_TAIL(&v->active_channels, vc, qlink);
    }

    vllp_send_pkt(v, v->current_tx);
    return 0;
  }

  if(now >= v->next_ack) {
    int interval = VLLP_ACK_INTERVAL * 0.9;
    interval += (VLLP_ACK_INTERVAL * (rand() % 200) / 1000);
    v->next_ack = now + interval;
    vllp_send_ack(v);
    return 0;
  }

  return MIN(v->next_rtx, v->next_ack);
}


static void *
vllp_thread(void *arg)
{
  vllp_t *v = arg;
  vllp_pkt_t *vp;
  int64_t timeout = 0;
  v->next_ack = 0;
  v->next_rtx = INT64_MAX;

  pthread_mutex_lock(&v->mutex);

  while(v->run) {
    const int64_t now = get_ts();

    if(v->connected) {
      handle_pending_channels(v, now);
    }

    if(timeout && now > timeout) {
      vllp_log(v, LOG_WARNING, "Timeout");
      vllp_disconnect(v, VLLP_ERR_TIMEOUT);
      timeout = 0;
      continue;
    }

    if(TAILQ_FIRST(&v->rxq) != NULL) {
      vp = TAILQ_FIRST(&v->rxq);
      TAILQ_REMOVE(&v->rxq, vp, link);
      if(vllp_handle_rx(v, vp, now) == 0) {
        timeout = now + v->timeout * 1000000;
      } else {
        vllp_disconnect(v, VLLP_ERR_MALFORMED);
      }
      free(vp);
      continue;
    }

    int64_t wakeup = vllp_tx(v, now);
    if(wakeup == 0)
      continue;

    if(wakeup == INT64_MAX) {
      pthread_cond_wait(&v->cond, &v->mutex);
    } else {
      struct timespec ts;
      ts.tv_sec  =  wakeup / 1000000;
      ts.tv_nsec = (wakeup % 1000000) * 1000;
      pthread_cond_timedwait(&v->cond, &v->mutex, &ts);
    }
  }

  vllp_channel_t *vc = v->cmc;

  if(vc->state == VLLP_CHANNEL_STATE_ACTIVE) {
    TAILQ_REMOVE(&v->active_channels, vc, qlink);
    vllp_channel_release(vc, "end-of-thread");
  }

  vllp_channel_unlink(vc);

  pthread_mutex_unlock(&v->mutex);

  vllp_channel_release(vc, "cmc ownership");

  return NULL;
}


static vllp_channel_t *
channel_make(vllp_t *v, int id, int state)
{
  vllp_channel_t *vc = calloc(1, sizeof(vllp_channel_t));
  __atomic_store_n(&vc->refcount, 2, __ATOMIC_SEQ_CST);
  vc->id = id;
  vc->state = state;
  TAILQ_INIT(&vc->rxq);
  TAILQ_INIT(&vc->txq);
  TAILQ_INIT(&vc->mtxq);
  LIST_INSERT_HEAD(&v->channels, vc, link);
  vc->vllp = v;
  vllp_retain(v);

  pthread_cond_init(&vc->state_cond, NULL);

  pthread_condattr_t cond_attr;
  pthread_condattr_init(&cond_attr);
#ifdef __linux__
  pthread_condattr_setclock(&cond_attr, CLOCK_MONOTONIC);
#endif
  pthread_cond_init(&vc->rxq_cond, &cond_attr);
  return vc;
}


static int
cmc_handle_open(vllp_t *v, int target_channel, const uint8_t *data, size_t len)
{
  if(len < 1) {
    vllp_log(v, LOG_ERR, "Channel_open message too short");
    return VLLP_ERR_MALFORMED;
  }

  if(channel_find(v, target_channel)) {
    vllp_log(v, LOG_ERR, "Channel already open");
    return VLLP_ERR_BAD_STATE;
  }

  char *name = alloca(len + 1);
  memcpy(name, data, len);
  name[len] = 0;

  vllp_channel_t *vc = channel_make(v, target_channel,
                                     VLLP_CHANNEL_STATE_CREATED);

  uint32_t iv = vllp_gen_channel_crc(v);
  vc->tx_crc_IV = iv;
  vc->rx_crc_IV = ~iv;

  open_channel_result_t r = v->open_channel(v->opaque, name, vc);

  if(!r.error) {
    vc->flags = 0;
    vllp_channel_set_state(vc, VLLP_CHANNEL_STATE_ESTABLISHED);
    vc->rx = r.rx;
    vc->eof = r.eof;
    vc->opaque = r.opaque;
  } else {
    vllp_channel_unlink(vc);
    vc->state = VLLP_CHANNEL_STATE_CLOSED;
    vllp_channel_release(vc, "open-failed");
  }

  uint8_t response[3];
  response[0] = target_channel | (VLLP_CMC_OPCODE_OPEN_RESPONSE << 4);
  response[1] = r.error;
  response[2] = r.error >> 8;
  channel_send_message(v, v->cmc, response, sizeof(response));
  return 0;
}

static int
cmc_handle_open_response(vllp_t *v, int target_channel,
                         const uint8_t *data, size_t len)
{
  vllp_channel_t *vc = channel_find(v, target_channel);

  if(vc == NULL) {
    vllp_log(v, LOG_ERR, "channel_open_ressponse on unknown channel");
    return VLLP_ERR_BAD_STATE;
  }

  if(vc->state != VLLP_CHANNEL_STATE_OPEN_SENT) {
    vllp_log(v, LOG_ERR, "channel_open_response on channel in unexpected state");
    return VLLP_ERR_BAD_STATE;
  }

  if(len != 2) {
    vllp_log(v, LOG_ERR, "channel_open_ressponse invalid length");
    return VLLP_ERR_MALFORMED;
  }
  const uint8_t *u8 = data;
  int16_t err = u8[0] | (u8[1] << 8);

  if(!err) {
    channel_enq_rx_meta(vc, 0, VLLP_PKT_RDY);
    if(TAILQ_FIRST(&vc->mtxq) != NULL) {
      vllp_channel_set_state(vc, VLLP_CHANNEL_STATE_ACTIVE);
      TAILQ_INSERT_TAIL(&v->active_channels, vc, qlink);
      vllp_channel_retain(vc, "active-after-open");
    } else {
      vllp_channel_set_state(vc, VLLP_CHANNEL_STATE_ESTABLISHED);
    }
  } else {
    vllp_channel_set_state(vc, VLLP_CHANNEL_STATE_CLOSED);

    v->available_channel_ids |= (1 << vc->id);
    LIST_REMOVE(vc, link);
    channel_enq_rx_meta(vc, err, VLLP_PKT_EOF);
    vllp_channel_release(vc, "remote-open-failed");
  }

  return 0;
}


static int
cmc_handle_close(vllp_t *v, int target_channel, const uint8_t *data, size_t len)
{
  if(len < 2) {
    vllp_log(v, LOG_ERR, "channel_close message too short");
    return VLLP_ERR_MALFORMED;
  }

  int16_t error_code = data[0] | (data[1] << 8);

  vllp_channel_t *vc = channel_find(v, target_channel);
  if(vc == 0)
    return 0;

  if(vc == v->cmc)
    return VLLP_ERR_MALFORMED;

  switch(vc->state) {
  case VLLP_CHANNEL_STATE_CREATED:
  case VLLP_CHANNEL_STATE_PENDING_OPEN:
  case VLLP_CHANNEL_STATE_OPEN_SENT:
    return 0;

  case VLLP_CHANNEL_STATE_ACTIVE:
  case VLLP_CHANNEL_STATE_ESTABLISHED:
    channel_send_close(v, vc, error_code);
    channel_enq_rx_meta(vc, error_code, VLLP_PKT_EOF);
    return 0;

  case VLLP_CHANNEL_STATE_CLOSE_SENT:
    vllp_channel_set_state(vc, VLLP_CHANNEL_STATE_CLOSED);
    if(is_client(v))
      v->available_channel_ids |= (1 << vc->id);
    LIST_REMOVE(vc, link);
    channel_enq_rx_meta(vc, error_code, VLLP_PKT_EOF);
    vllp_channel_release(vc, __FUNCTION__);
    return 0;

  case VLLP_CHANNEL_STATE_CLOSED:
    return 0;
  }

  return 0;
}



static int
cmc_rx(void *opaque, const void *data, size_t len)
{
  vllp_t *v = opaque;
  if(len < 1) {
    vllp_log(v, LOG_ERR, "channel_control message too short");
    return VLLP_ERR_MALFORMED;
  }

  const uint8_t *u8 = data;
  uint8_t opcode = u8[0] >> 4;
  uint8_t channel = u8[0] & 0xf;

  if(is_client(v)) {

    switch(opcode) {
    case VLLP_CMC_OPCODE_OPEN_RESPONSE:
      return cmc_handle_open_response(v, channel, data + 1, len - 1);
    case VLLP_CMC_OPCODE_CLOSE:
      return cmc_handle_close(v, channel, data + 1, len - 1);
    default:
      vllp_log(v, LOG_ERR, "channel_control client unexpected opcode");
      return VLLP_ERR_MALFORMED;
    }

  } else {

    switch(opcode) {
    case VLLP_CMC_OPCODE_OPEN:
      return cmc_handle_open(v, channel, data + 1, len - 1);
    case VLLP_CMC_OPCODE_CLOSE:
      return cmc_handle_close(v, channel, data + 1, len - 1);

    default:
      vllp_log(v, LOG_ERR, "channel_control server unexpected opcode");
      return VLLP_ERR_MALFORMED;
    }
  }

  return 0;
}


static vllp_t *
vllp_create(int mtu, int timeout, uint32_t flags, void *opaque,
             void (*tx)(void *opaque, const void *data, size_t len),
            void (*log)(void *opaque, int level, const char *msg))
{
  if(mtu < 8)
    return NULL;

  vllp_t *v = calloc(1, sizeof(vllp_t));
  v->run = 1;
  __atomic_store_n(&v->refcount, 1, __ATOMIC_SEQ_CST);
  v->remote_flow_status = 0xffff;
  v->local_flow_status = 0xffff;
  v->flags = flags;
  v->tx = tx;
  v->log = log;
  v->opaque = opaque;
  v->timeout = timeout;
  if(flags & VLLP_FDCAN_ADAPTATION && mtu > 8)
    mtu--;
  v->mtu = mtu;
  v->SE = VLLP_HDR_E;

  TAILQ_INIT(&v->rxq);
  TAILQ_INIT(&v->pending_open);
  TAILQ_INIT(&v->active_channels);

  v->cmc = channel_make(v, 14, VLLP_CHANNEL_STATE_ESTABLISHED);

  pthread_mutex_init(&v->mutex, NULL);

  pthread_condattr_t cond_attr;
  pthread_condattr_init(&cond_attr);
#ifdef __linux__
  pthread_condattr_setclock(&cond_attr, CLOCK_MONOTONIC);
#endif
  pthread_cond_init(&v->cond, &cond_attr);
  return v;
}



vllp_t *
vllp_create_client(int mtu, int timeout, uint32_t flags, void *opaque,
                   void (*tx)(void *opaque, const void *data,
                              size_t len),
                   void (*log)(void *opaque, int level, const char *msg))
{
  vllp_t *v = vllp_create(mtu, timeout, flags, opaque, tx, log);
  v->available_channel_ids = 0x3fff; // channel 14 and 15 are not for user
  return v;
}


vllp_t *
vllp_create_server(int mtu, int timeout, uint32_t flags, void *opaque,
                   void (*tx)(void *opaque, const void *data, size_t len),
                   void (*log)(void *opaque, int level, const char *msg),
                   open_channel_result_t (*open_channel)(void *opaque,
                                                         const char *name,
                                                         vllp_channel_t *vc))
{
  if(open_channel == NULL)
    return NULL;

  vllp_t *v = vllp_create(mtu, timeout, flags, opaque, tx, log);
  v->open_channel = open_channel;
  return v;
}

void
vllp_start(vllp_t *v)
{
  pthread_create(&v->tid, NULL, vllp_thread, v);
}


void
vllp_input(vllp_t *v, const void *data, size_t len)
{
  if(v->flags & VLLP_FDCAN_ADAPTATION && len > 8) {
    const uint8_t *u8 = data;
    int pad = u8[len - 1];
    if(pad >= len)
      return;
    len -= pad;
  }

  if(len < 1)
    return;

  vllp_pkt_t *vp = malloc(sizeof(vllp_pkt_t) + len);
  memcpy(vp->data, data, len);
  vp->len = len;
  vp->type = 0;
  pthread_mutex_lock(&v->mutex);
  TAILQ_INSERT_TAIL(&v->rxq, vp, link);
  pthread_cond_signal(&v->cond);
  pthread_mutex_unlock(&v->mutex);
}


static void *
vllp_channel_rx_thread(void *arg)
{
  vllp_channel_t *vc = arg;
  vllp_t *v = vc->vllp;
  vllp_pkt_t *vp;

  pthread_mutex_lock(&v->mutex);

  while(vc->rx_thread_run) {
    vp = TAILQ_FIRST(&vc->rxq);
    if(vp == NULL) {
      pthread_cond_wait(&vc->rxq_cond, &v->mutex);
      continue;
    }

    TAILQ_REMOVE(&vc->rxq, vp, link);

    if(vc->eof != NULL) {

      if(vp->type == VLLP_PKT_EOF) {
        int error_code;
        memcpy(&error_code, vp->data, sizeof(int));

        pthread_mutex_unlock(&v->mutex);
        vc->eof(vc->opaque, error_code);
        pthread_mutex_lock(&v->mutex);

        if(!(vc->flags & VLLP_CHANNEL_RECONNECT)) {
          vc->eof = NULL;
        }

      } else if(vp->type == VLLP_PKT_RDY) {

        pthread_mutex_unlock(&v->mutex);
        if(vc->rdy != NULL)
          vc->rdy(vc->opaque);
        pthread_mutex_lock(&v->mutex);

      } else {
        pthread_mutex_unlock(&v->mutex);
        vc->rx(vc->opaque, vp->data, vp->len);
        pthread_mutex_lock(&v->mutex);
      }
    }
    free(vp);
  }
  pthread_mutex_unlock(&v->mutex);
  return NULL;
}


void
vllp_destroy(vllp_t *v)
{
  pthread_mutex_lock(&v->mutex);
  v->run = 0;
  pthread_cond_signal(&v->cond);
  pthread_mutex_unlock(&v->mutex);

  pthread_join(v->tid, NULL);
  vllp_release(v);
}

static void
vllp_retain(vllp_t *v)
{
  __sync_add_and_fetch(&v->refcount, 1);
}

static void
vllp_release(vllp_t *v)
{
  if(__sync_add_and_fetch(&v->refcount, -1))
    return;

  assert(LIST_FIRST(&v->channels) == NULL);
  assert(TAILQ_FIRST(&v->pending_open) == NULL);
  assert(TAILQ_FIRST(&v->active_channels) == NULL);

  vllp_pkt_t *vp, *n;
  for(vp = TAILQ_FIRST(&v->rxq); vp != NULL; vp = n) {
    n = TAILQ_NEXT(vp, link);
    free(vp);
  }
  free(v->current_tx);
  free(v);
}



vllp_channel_t *
vllp_channel_create(vllp_t *v, const char *name, uint32_t flags,
                    void (*rx)(void *opaque, const void *data, size_t length),
                    void (*eof)(void *opaque, int error_code),
                    void (*rdy)(void *opaque),
                    void *opaque)
{
  vllp_channel_t *vc = NULL;

  if(is_server(v))
    return NULL;

  if(!rx != !eof)
    return NULL;   // Both must be set or cleared

  pthread_mutex_lock(&v->mutex);

  if(v->available_channel_ids) {
    vc = channel_make(v, ffs(v->available_channel_ids) - 1,
                      VLLP_CHANNEL_STATE_PENDING_OPEN);
    vc->rx = rx;
    vc->eof = eof;
    vc->rdy = rdy;
    vc->opaque = opaque;
    vc->name = strdup(name);
    vc->flags = flags;
    v->available_channel_ids &= ~(1 << vc->id);
    TAILQ_INSERT_TAIL(&v->pending_open, vc, qlink);
    vllp_channel_retain(vc, "initial-pending-open");
    pthread_cond_signal(&v->cond);

    if(rx) {
      vllp_channel_retain(vc, "rx-dispatch");
      vc->rx_thread_run = 1;
      pthread_create(&vc->rx_thread, NULL, vllp_channel_rx_thread, vc);
    }

  }

  pthread_mutex_unlock(&v->mutex);
  return vc;
}


void
vllp_channel_send(vllp_channel_t *vc, const void *data, size_t len)
{
  vllp_t *v = vc->vllp;

  pthread_mutex_lock(&v->mutex);
  channel_send_message(v, vc, data, len);
  pthread_cond_signal(&v->cond);
  pthread_mutex_unlock(&v->mutex);
}

void
vllp_channel_close(vllp_channel_t *vc, int error_code, int wait)
{
  vllp_t *v = vc->vllp;
  pthread_mutex_lock(&v->mutex);

  if(vc->rx_thread_run) {

    vc->rx_thread_run = 0;
    pthread_cond_signal(&vc->rxq_cond);
    pthread_mutex_unlock(&v->mutex);

    int err = pthread_join(vc->rx_thread, NULL);
    if(err) {
      fprintf(stderr, "vllp_channel_close -- %s\n", strerror(err));
      abort();
    }
    pthread_mutex_lock(&v->mutex);
    vllp_channel_release(vc, "rx-thread-closed");
  }

  switch(vc->state) {
  case VLLP_CHANNEL_STATE_CREATED:
    abort();

  case VLLP_CHANNEL_STATE_PENDING_OPEN:
    TAILQ_REMOVE(&v->pending_open, vc, qlink);
    if(is_client(v))
      v->available_channel_ids |= (1 << vc->id);
    vllp_channel_release(vc, "close-while-pending-open");
    vllp_channel_unlink(vc);
    vllp_channel_release(vc, "close"); // Reference held by our caller
    pthread_mutex_unlock(&v->mutex);
    return;

  case VLLP_CHANNEL_STATE_OPEN_SENT:
  case VLLP_CHANNEL_STATE_ESTABLISHED:
  case VLLP_CHANNEL_STATE_ACTIVE:
    channel_send_close(v, vc, error_code);
    pthread_cond_signal(&v->cond);
    break;

  case VLLP_CHANNEL_STATE_CLOSE_SENT:
    vllp_channel_unlink(vc);
    break;
  case VLLP_CHANNEL_STATE_CLOSED:
    break;
  }

  if(wait) {
    while(vc->state != VLLP_CHANNEL_STATE_CLOSED) {
      pthread_cond_wait(&vc->state_cond, &v->mutex);
    }
  }

  pthread_mutex_unlock(&v->mutex);

  vllp_channel_release(vc, "close"); // Reference held by our caller
}


int
vllp_channel_read(vllp_channel_t *vc, void **data, size_t *lenp, long timeout)
{
  int err;
  vllp_t *v = vc->vllp;

  vllp_pkt_t *vp;

  pthread_mutex_lock(&v->mutex);

  if(vc->is_closed) {
    err = vc->closed_status;
    *data = NULL;
    *lenp = 0;
    pthread_mutex_unlock(&v->mutex);
    return err;
  }

  int64_t deadline = timeout >= 0 ? get_ts() + timeout : 0;

  while(1) {

    if(deadline == 0) {
      while((vp = TAILQ_FIRST(&vc->rxq)) == NULL) {
        pthread_cond_wait(&vc->rxq_cond, &v->mutex);
        continue;
      }
    } else {

      int64_t deadline = get_ts() + timeout;

      struct timespec ts;
      ts.tv_sec  =  deadline / 1000000;
      ts.tv_nsec = (deadline % 1000000) * 1000;

      while((vp = TAILQ_FIRST(&vc->rxq)) == NULL) {
        if(pthread_cond_timedwait(&vc->rxq_cond, &v->mutex, &ts) == ETIMEDOUT) {
          pthread_mutex_unlock(&v->mutex);
          *data = NULL;
          *lenp = 0;
          return VLLP_ERR_TIMEOUT;
        }
        continue;
      }
    }

    TAILQ_REMOVE(&vc->rxq, vp, link);

    if(vp->type == VLLP_PKT_EOF) {
      memcpy(&err, vp->data, sizeof(int));
      *data = NULL;
      *lenp = 0;
      vc->is_closed = 1;
      vc->closed_status = err;
    } else if(vp->type == VLLP_PKT_RDY) {
      free(vp);
      continue;
    } else {
      void *c = malloc(vp->len);
      memcpy(c, vp->data, vp->len);
      *data = c;
      *lenp = vp->len;
      err = 0;
    }
    break;
  }
  pthread_mutex_unlock(&v->mutex);
  free(vp);
  return err;
}


static const uint32_t crc32table[256] = {
    0xd202ef8d, 0xa505df1b, 0x3c0c8ea1, 0x4b0bbe37, 0xd56f2b94, 0xa2681b02,
    0x3b614ab8, 0x4c667a2e, 0xdcd967bf, 0xabde5729, 0x32d70693, 0x45d03605,
    0xdbb4a3a6, 0xacb39330, 0x35bac28a, 0x42bdf21c, 0xcfb5ffe9, 0xb8b2cf7f,
    0x21bb9ec5, 0x56bcae53, 0xc8d83bf0, 0xbfdf0b66, 0x26d65adc, 0x51d16a4a,
    0xc16e77db, 0xb669474d, 0x2f6016f7, 0x58672661, 0xc603b3c2, 0xb1048354,
    0x280dd2ee, 0x5f0ae278, 0xe96ccf45, 0x9e6bffd3, 0x0762ae69, 0x70659eff,
    0xee010b5c, 0x99063bca, 0x000f6a70, 0x77085ae6, 0xe7b74777, 0x90b077e1,
    0x09b9265b, 0x7ebe16cd, 0xe0da836e, 0x97ddb3f8, 0x0ed4e242, 0x79d3d2d4,
    0xf4dbdf21, 0x83dcefb7, 0x1ad5be0d, 0x6dd28e9b, 0xf3b61b38, 0x84b12bae,
    0x1db87a14, 0x6abf4a82, 0xfa005713, 0x8d076785, 0x140e363f, 0x630906a9,
    0xfd6d930a, 0x8a6aa39c, 0x1363f226, 0x6464c2b0, 0xa4deae1d, 0xd3d99e8b,
    0x4ad0cf31, 0x3dd7ffa7, 0xa3b36a04, 0xd4b45a92, 0x4dbd0b28, 0x3aba3bbe,
    0xaa05262f, 0xdd0216b9, 0x440b4703, 0x330c7795, 0xad68e236, 0xda6fd2a0,
    0x4366831a, 0x3461b38c, 0xb969be79, 0xce6e8eef, 0x5767df55, 0x2060efc3,
    0xbe047a60, 0xc9034af6, 0x500a1b4c, 0x270d2bda, 0xb7b2364b, 0xc0b506dd,
    0x59bc5767, 0x2ebb67f1, 0xb0dff252, 0xc7d8c2c4, 0x5ed1937e, 0x29d6a3e8,
    0x9fb08ed5, 0xe8b7be43, 0x71beeff9, 0x06b9df6f, 0x98dd4acc, 0xefda7a5a,
    0x76d32be0, 0x01d41b76, 0x916b06e7, 0xe66c3671, 0x7f6567cb, 0x0862575d,
    0x9606c2fe, 0xe101f268, 0x7808a3d2, 0x0f0f9344, 0x82079eb1, 0xf500ae27,
    0x6c09ff9d, 0x1b0ecf0b, 0x856a5aa8, 0xf26d6a3e, 0x6b643b84, 0x1c630b12,
    0x8cdc1683, 0xfbdb2615, 0x62d277af, 0x15d54739, 0x8bb1d29a, 0xfcb6e20c,
    0x65bfb3b6, 0x12b88320, 0x3fba6cad, 0x48bd5c3b, 0xd1b40d81, 0xa6b33d17,
    0x38d7a8b4, 0x4fd09822, 0xd6d9c998, 0xa1def90e, 0x3161e49f, 0x4666d409,
    0xdf6f85b3, 0xa868b525, 0x360c2086, 0x410b1010, 0xd80241aa, 0xaf05713c,
    0x220d7cc9, 0x550a4c5f, 0xcc031de5, 0xbb042d73, 0x2560b8d0, 0x52678846,
    0xcb6ed9fc, 0xbc69e96a, 0x2cd6f4fb, 0x5bd1c46d, 0xc2d895d7, 0xb5dfa541,
    0x2bbb30e2, 0x5cbc0074, 0xc5b551ce, 0xb2b26158, 0x04d44c65, 0x73d37cf3,
    0xeada2d49, 0x9ddd1ddf, 0x03b9887c, 0x74beb8ea, 0xedb7e950, 0x9ab0d9c6,
    0x0a0fc457, 0x7d08f4c1, 0xe401a57b, 0x930695ed, 0x0d62004e, 0x7a6530d8,
    0xe36c6162, 0x946b51f4, 0x19635c01, 0x6e646c97, 0xf76d3d2d, 0x806a0dbb,
    0x1e0e9818, 0x6909a88e, 0xf000f934, 0x8707c9a2, 0x17b8d433, 0x60bfe4a5,
    0xf9b6b51f, 0x8eb18589, 0x10d5102a, 0x67d220bc, 0xfedb7106, 0x89dc4190,
    0x49662d3d, 0x3e611dab, 0xa7684c11, 0xd06f7c87, 0x4e0be924, 0x390cd9b2,
    0xa0058808, 0xd702b89e, 0x47bda50f, 0x30ba9599, 0xa9b3c423, 0xdeb4f4b5,
    0x40d06116, 0x37d75180, 0xaede003a, 0xd9d930ac, 0x54d13d59, 0x23d60dcf,
    0xbadf5c75, 0xcdd86ce3, 0x53bcf940, 0x24bbc9d6, 0xbdb2986c, 0xcab5a8fa,
    0x5a0ab56b, 0x2d0d85fd, 0xb404d447, 0xc303e4d1, 0x5d677172, 0x2a6041e4,
    0xb369105e, 0xc46e20c8, 0x72080df5, 0x050f3d63, 0x9c066cd9, 0xeb015c4f,
    0x7565c9ec, 0x0262f97a, 0x9b6ba8c0, 0xec6c9856, 0x7cd385c7, 0x0bd4b551,
    0x92dde4eb, 0xe5dad47d, 0x7bbe41de, 0x0cb97148, 0x95b020f2, 0xe2b71064,
    0x6fbf1d91, 0x18b82d07, 0x81b17cbd, 0xf6b64c2b, 0x68d2d988, 0x1fd5e91e,
    0x86dcb8a4, 0xf1db8832, 0x616495a3, 0x1663a535, 0x8f6af48f, 0xf86dc419,
    0x660951ba, 0x110e612c, 0x88073096, 0xff000000
};


uint32_t
vllp_crc32(uint32_t crc, const void *data, size_t n_bytes)
{
  for (size_t i = 0; i < n_bytes; ++i)
    crc = crc32table[(uint8_t)crc ^ ((uint8_t*)data)[i]] ^ crc >> 8;

  return crc;
}


static const char errmsg[] = {
  "OK\0"
  "NOT_IMPLEMENTED\0"
  "TIMEOUT\0"
  "OPERATION_FAILED\0"
  "TX_FAULT\0"
  "RX_FAULT\0"
  "NOT_READY\0"
  "NO_BUFFER\0"
  "MTU_EXCEEDED\0"
  "INVALID_ID\0"
  "DMAXFER\0"
  "BUS_ERR\0"
  "ARBITRATION_LOST\0"
  "BAD_STATE\0"
  "INVALID_ADDRESS\0"
  "NO_DEVICE\0"
  "MISMATCH\0"
  "NOT_FOUND\0"
  "CHECKSUM_ERR\0"
  "MALFORMED\0"
  "INVALID_RPC_ID\0"
  "INVALID_RPC_ARGS\0"
  "NO_FLASH_SPACE\0"
  "INVALID_ARGS\0"
  "INVALID_LENGTH\0"
  "NOT_IDLE\0"
  "BAD_CONFIG\0"
  "FLASH_HW_ERR\0"
  "FLASH_TIMEOUT\0"
  "NO_MEMORY\0"
  "READ_PROT\0"
  "WRITE_PROT\0"
  "AGAIN\0"
  "NOT_CONNECTED\0"
  "BAD_PKT_SIZ\0"
  "EXISTS\0"
  "CORRUPT\0"
  "NOT_DIR\0"
  "IS_DIR\0"
  "NOT_EMPTY\0"
  "BADF\0"
  "TOOBIG\0"
  "INVALID_PARAMETER\0"
  "NOTATTR\0"
  "TOOLONG\0"
  "IO\0"
  "FS\0"
  "DMAFIFO\0"
  "INTERRUPTED\0"
  "QUEUE_FULL\0"
  "NO_ROUTE\0"
  "\0"
};


static const char *
strtbl(const char *str, size_t index)
{
  while(1) {
    if(!index)
      return str;
    index--;
    size_t n = strlen(str);
    if(n == 0)
      return "???";
    str += n + 1;
  }
}

const char *
vllp_strerror(int error)
{
  return strtbl(errmsg, -error);
}



void
vllp_logf(vllp_t *v, int level, const char *fmt, ...)
{
  va_list ap;
  va_start(ap, fmt);
  char *out = NULL;
  if(vasprintf(&out, fmt, ap) < 0)
    out = NULL;
  va_end(ap);
  if(out)
    vllp_log(v, level, out);
  free(out);
}
