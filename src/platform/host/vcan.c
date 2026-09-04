#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/param.h>

#include <mios/mios.h>
#include <mios/bytestream.h>

#include <net/pbuf.h>
#include <net/can/can.h>

#include "irq.h"
#include "sim.h"
#include "vcan.h"

#define VCAN_MAX_PAYLOAD 64
#define VCAN_RING_SIZE   64   // power of two

typedef struct vcan_slot {
  uint32_t id;
  uint16_t len;
  uint8_t data[VCAN_MAX_PAYLOAD];
} vcan_slot_t;

// Single producer, single consumer. Producer and consumer never run at
// the same time (lockstep), so plain counters suffice.
typedef struct vcan_ring {
  volatile uint32_t rd;
  volatile uint32_t wr;
  uint32_t drops;
  vcan_slot_t slot[VCAN_RING_SIZE];
} vcan_ring_t;

struct vcan {
  can_netif_t v_cni;
  vcan_ring_t v_tx;          // mios -> peer
  vcan_ring_t v_rx;          // peer -> mios
  sim_thread_t *v_peer;      // whoever called vcan_peer_recv() last
  int v_irq;
};


static int
ring_put(vcan_ring_t *r, uint32_t id, const void *payload, size_t len)
{
  if(len > VCAN_MAX_PAYLOAD || r->wr - r->rd == VCAN_RING_SIZE) {
    r->drops++;
    return 0;
  }
  vcan_slot_t *s = &r->slot[r->wr & (VCAN_RING_SIZE - 1)];
  s->id = id;
  memcpy(s->data, payload, len);
  s->len = len;
  __atomic_store_n(&r->wr, r->wr + 1, __ATOMIC_SEQ_CST);
  return 1;
}

static const vcan_slot_t *
ring_peek(vcan_ring_t *r)
{
  if(r->rd == r->wr)
    return NULL;
  return &r->slot[r->rd & (VCAN_RING_SIZE - 1)];
}

static void
ring_pop(vcan_ring_t *r)
{
  __atomic_store_n(&r->rd, r->rd + 1, __ATOMIC_SEQ_CST);
}


// ---- mios side ----

static void
vcan_print_info(struct device *dev, struct stream *st)
{
  vcan_t *v = (vcan_t *)dev;
  stprintf(st, "vcan mtu %d  ring drops: tx %u rx %u\n",
           v->v_cni.cni_ni.ni_mtu, v->v_tx.drops, v->v_rx.drops);
}

static const device_class_t vcan_device_class = {
  .dc_class_name = "vcan",
  .dc_print_info = vcan_print_info,
};


// DSIG output: (id, prefix-less payload). Store it for the peer. Not
// consumed -- return the pbuf so the dsig send path frees it.
static pbuf_t *
vcan_output(can_netif_t *cni, pbuf_t *pb, uint32_t id)
{
  vcan_t *v = (vcan_t *)cni;

  if(pbuf_pullup(pb, pb->pb_pktlen))
    return pb;

  if(!ring_put(&v->v_tx, id, pbuf_cdata(pb, 0), pb->pb_pktlen))
    ; // ring full: dropped, counted in v_tx.drops

  if(v->v_peer != NULL)
    sim_post(v->v_peer);
  return pb;
}


// IRQ_LEVEL_NET: move injected frames from the RX ring into pbufs,
// prefixed with the 4-byte LE signal id that can_input() expects.
static void
vcan_irq(void *arg)
{
  vcan_t *v = arg;
  const vcan_slot_t *s;
  int wakeup = 0;

  while((s = ring_peek(&v->v_rx)) != NULL) {
    const size_t total = s->len + 4;
    // pbuf_make() allocates both the pbuf and its data buffer; write the
    // framed [u32 LE id][payload] straight into that buffer.
    pbuf_t *pb = pbuf_make(0, 0);
    if(pb == NULL)
      break; // no buffers: leave it in the ring, retry on the next IRQ
    uint8_t *d = pbuf_data(pb, 0);
    wr32_le(d, s->id);
    memcpy(d + 4, s->data, s->len);
    pb->pb_buflen = total;
    pb->pb_pktlen = total;
    pb->pb_flags = PBUF_SOP | PBUF_EOP;
    STAILQ_INSERT_TAIL(&v->v_cni.cni_ni.ni_rx_queue, pb, pb_link);
    ring_pop(&v->v_rx);
    wakeup = 1;
  }
  if(wakeup)
    netif_wakeup(&v->v_cni.cni_ni);
}


void
vcan_set_link(vcan_t *v, int up)
{
  net_task_raise(&v->v_cni.cni_ni.ni_task,
                 up ? NETIF_TASK_STATUS_UP : NETIF_TASK_STATUS_DOWN);
}


can_netif_t *
vcan_cni(vcan_t *v)
{
  return &v->v_cni;
}


vcan_t *
vcan_create(const char *name, uint8_t mtu)
{
  vcan_t *v = calloc(1, sizeof(vcan_t));
  v->v_cni.cni_output = vcan_output;
  v->v_irq = host_irq_alloc(IRQ_LEVEL_NET, vcan_irq, v);
  can_netif_attach(&v->v_cni, name, &vcan_device_class, NULL);
  v->v_cni.cni_ni.ni_mtu = mtu;
  v->v_cni.cni_ni.ni_flags |= NETIF_F_UP;
  return v;
}


// ---- peer side ----

ssize_t
vcan_peer_recv(vcan_t *v, uint32_t *id, void *buf, size_t buflen,
               uint64_t deadline)
{
  sim_thread_t *me = sim_current();
  if(me == NULL)
    panic("vcan_peer_recv() must be called from a simulation thread");
  v->v_peer = me;

  while(1) {
    const vcan_slot_t *s = ring_peek(&v->v_tx);
    if(s != NULL) {
      const size_t len = s->len < buflen ? s->len : buflen;
      if(id != NULL)
        *id = s->id;
      memcpy(buf, s->data, len);
      ring_pop(&v->v_tx);
      return len;
    }
    if(!sim_wait(deadline))
      return -1;
  }
}


void
vcan_peer_send(vcan_t *v, uint32_t id, const void *payload, size_t len)
{
  ring_put(&v->v_rx, id, payload, len);
  host_irq_pend(v->v_irq);
}


ssize_t
vcan_peer_poll(vcan_t *v, uint32_t *id, void *buf, size_t buflen)
{
  const vcan_slot_t *s = ring_peek(&v->v_tx);
  if(s == NULL)
    return -1;
  const size_t len = s->len < buflen ? s->len : buflen;
  if(id != NULL)
    *id = s->id;
  memcpy(buf, s->data, len);
  ring_pop(&v->v_tx);
  return len;
}
