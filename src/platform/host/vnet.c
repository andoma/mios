#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <mios/mios.h>
#include <mios/task.h>

#include <net/pbuf.h>
#include <net/ether.h>

#include "irq.h"
#include "sim.h"
#include "vnet.h"
#include "hostnet.h"

#define VNET_MAX_FRAME 2048
#define VNET_RING_SIZE 32   // power of two

typedef struct vnet_slot {
  uint16_t len;
  uint8_t data[VNET_MAX_FRAME];
} vnet_slot_t;

// Single producer, single consumer. Producer and consumer never run at
// the same time (lockstep), so plain counters suffice.
typedef struct vnet_ring {
  volatile uint32_t rd;
  volatile uint32_t wr;
  uint32_t drops;
  vnet_slot_t slot[VNET_RING_SIZE];
} vnet_ring_t;

struct vnet {
  ether_netif_t v_eni;
  vnet_ring_t v_tx;          // Mios -> peer
  vnet_ring_t v_rx;          // peer -> Mios
  sim_thread_t *v_peer;      // whoever called vnet_peer_recv() last
  int v_irq;
  uint8_t v_txframe[VNET_MAX_FRAME];   // linearized outgoing frame
};


static int
ring_put(vnet_ring_t *r, const void *frame, size_t len)
{
  if(len > VNET_MAX_FRAME || r->wr - r->rd == VNET_RING_SIZE) {
    r->drops++;
    return 0;
  }
  vnet_slot_t *s = &r->slot[r->wr & (VNET_RING_SIZE - 1)];
  memcpy(s->data, frame, len);
  s->len = len;
  __atomic_store_n(&r->wr, r->wr + 1, __ATOMIC_SEQ_CST);
  return 1;
}

static const vnet_slot_t *
ring_peek(vnet_ring_t *r)
{
  if(r->rd == r->wr)
    return NULL;
  return &r->slot[r->rd & (VNET_RING_SIZE - 1)];
}

static void
ring_pop(vnet_ring_t *r)
{
  __atomic_store_n(&r->rd, r->rd + 1, __ATOMIC_SEQ_CST);
}


// ---- Mios side ----

static void
vnet_print_info(struct device *dev, struct stream *st)
{
  vnet_t *v = (vnet_t *)dev;
  ether_print(&v->v_eni, st);
  stprintf(st, "Ring drops: tx %u  rx %u\n", v->v_tx.drops, v->v_rx.drops);
}

static const ethmac_device_class_t vnet_device_class = {
  .dc = {
    .dc_class_name = "vnet",
    .dc_print_info = vnet_print_info,
  }
};


static error_t
vnet_output(struct ether_netif *eni, pbuf_t *pkt,
            pbuf_tx_cb_t *txcb, uint32_t id)
{
  vnet_t *v = (vnet_t *)eni;
  uint8_t *frame = v->v_txframe;

  if(pkt->pb_pktlen > VNET_MAX_FRAME) {
    eni->eni_stats.tx_qdrop++;
    pbuf_free(pkt);
    return ERR_MTU_EXCEEDED;
  }

  size_t len = 0;
  for(pbuf_t *pb = pkt; pb != NULL; pb = pb->pb_next) {
    memcpy(frame + len, pbuf_data(pb, 0), pb->pb_buflen);
    len += pb->pb_buflen;
  }
  pbuf_free(pkt);

  eni->eni_stats.tx_pkt++;
  eni->eni_stats.tx_byte += len;

  if(!ring_put(&v->v_tx, frame, len))
    eni->eni_stats.tx_qdrop++;

  if(v->v_peer != NULL)
    sim_post(v->v_peer);
  return 0;
}


// IRQ_LEVEL_NET: move injected frames from the RX ring into pbufs
static void
vnet_irq(void *arg)
{
  vnet_t *v = arg;
  const vnet_slot_t *s;
  int wakeup = 0;
  while((s = ring_peek(&v->v_rx)) != NULL) {
    host_ether_rx(&v->v_eni, s->data, s->len);
    ring_pop(&v->v_rx);
    wakeup = 1;
  }
  if(wakeup)
    netif_wakeup(&v->v_eni.eni_ni);
}


void
vnet_set_link(vnet_t *v, int up)
{
  net_task_raise(&v->v_eni.eni_ni.ni_task,
                 up ? NETIF_TASK_STATUS_UP : NETIF_TASK_STATUS_DOWN);
}


struct ether_netif *
vnet_eni(vnet_t *v)
{
  return &v->v_eni;
}


vnet_t *
vnet_create(const char *name, const uint8_t mac[6])
{
  vnet_t *v = calloc(1, sizeof(vnet_t));
  memcpy(v->v_eni.eni_addr, mac, 6);
  v->v_eni.eni_output = vnet_output;
  v->v_irq = host_irq_alloc(IRQ_LEVEL_NET, vnet_irq, v);
  ether_netif_init(&v->v_eni, name, &vnet_device_class);
  ether_netif_attach(&v->v_eni);
  return v;
}


// ---- Peer side ----

ssize_t
vnet_peer_recv(vnet_t *v, void *buf, size_t buflen, uint64_t deadline)
{
  sim_thread_t *me = sim_current();
  if(me == NULL)
    panic("vnet_peer_recv() must be called from a simulation thread");
  v->v_peer = me;

  while(1) {
    const vnet_slot_t *s = ring_peek(&v->v_tx);
    if(s != NULL) {
      const size_t len = s->len < buflen ? s->len : buflen;
      memcpy(buf, s->data, len);
      ring_pop(&v->v_tx);
      return len;
    }
    if(!sim_wait(deadline))
      return -1;
  }
}


void
vnet_peer_send(vnet_t *v, const void *frame, size_t len)
{
  ring_put(&v->v_rx, frame, len);
  host_irq_pend(v->v_irq);
}
