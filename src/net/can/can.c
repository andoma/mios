#include "can.h"

#include <assert.h>

#include <mios/bytestream.h>
#include <mios/dsig.h>

static can_netif_t *g_can_netif;


struct pbuf *
can_input(struct netif *ni, struct pbuf *pb)
{
  if(pb->pb_pktlen < 4)
    return pb;

  if(pbuf_pullup(pb, pb->pb_pktlen))
    return pb;

  const void *data = pbuf_cdata(pb, 0);
  uint32_t signal = rd32_le(data);

  dsig_dispatch(signal, data + 4, pb->pb_buflen - 4);
  return pb;
}

void
can_netif_attach(can_netif_t *cni, const char *name,
                 const device_class_t *dc)
{
  assert(g_can_netif == NULL);
  g_can_netif = cni;

  cni->cni_ni.ni_input = can_input;
  cni->cni_ni.ni_mtu = 8;

  netif_attach(&cni->cni_ni, name, dc);
}

pbuf_t *
can_output(pbuf_t *pb)
{
  if(g_can_netif == NULL) {
    return pb;
  }

  struct netif *ni = &g_can_netif->cni_ni;
  return ni->ni_output(ni, NULL, pb);
}

static mutex_t can_send_mutex = MUTEX_INITIALIZER("cansend");

static struct pbuf_queue can_send_queue =
  STAILQ_HEAD_INITIALIZER(can_send_queue);

static void
can_send_cb(net_task_t *nt, uint32_t signals)
{
  while(1) {
    mutex_lock(&can_send_mutex);
    pbuf_t *pb = pbuf_splice(&can_send_queue);
    mutex_unlock(&can_send_mutex);
    if(pb == NULL)
      break;
    pb = can_output(pb);
    if(pb != NULL)
      pbuf_free(pb);
  }
}

static net_task_t can_send_task = { can_send_cb };

void
can_send(pbuf_t *pb)
{
  mutex_lock(&can_send_mutex);
  int empty = !STAILQ_FIRST(&can_send_queue);
  STAILQ_INSERT_TAIL(&can_send_queue, pb, pb_link);
  mutex_unlock(&can_send_mutex);
  if(empty) {
    net_task_raise(&can_send_task, 1);
  }
}

void
can_dsig_emit(uint32_t signal, const void *data, size_t len)
{
  if(len > PBUF_DATA_SIZE - 4)
    return;

  pbuf_t *pb = pbuf_make(0, 0);
  if(pb == NULL)
    return;

  uint8_t *pkt = pbuf_data(pb, 0);
  wr32_le(pkt, signal);
  memcpy(pkt + 4, data, len);

  pb->pb_pktlen = len + 4;
  pb->pb_buflen = len + 4;

  can_send(pb);
}
