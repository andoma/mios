#include "mbus_dsig.h"

#include <mios/dsig.h>
#include <mios/task.h>

#include "net/pbuf.h"
#include "net/net_task.h"

#include <string.h>

#include <stdio.h>

#include "mbus.h"

struct pbuf *
mbus_dsig_input(struct pbuf *pb, uint16_t signal)
{
  if((pb = pbuf_pullup(pb, pb->pb_pktlen)) == NULL)
    return pb;
  // -6 accounts for header and trailing CRC
  dsig_dispatch(signal, pbuf_cdata(pb, 2), pb->pb_pktlen - 6);
  return pb;
}

static mutex_t dsig_emit_mutex = MUTEX_INITIALIZER("dsigemit");

static struct pbuf_queue dsig_emit_queue =
  STAILQ_HEAD_INITIALIZER(dsig_emit_queue);

static void
mbus_dsig_emit_cb(net_task_t *nt, uint32_t signals)
{
  while(1) {
    mutex_lock(&dsig_emit_mutex);
    pbuf_t *pb = pbuf_splice(&dsig_emit_queue);
    mutex_unlock(&dsig_emit_mutex);
    if(pb == NULL)
      break;
    pb = mbus_output(pb, 0xff);
    if(pb != NULL)
      pbuf_free(pb);
  }
}

static net_task_t mbus_dsig_emit_task = { mbus_dsig_emit_cb };

void
mbus_dsig_emit(uint16_t signal, const void *data, size_t len)
{
  if(len > PBUF_DATA_SIZE - 2)
    return;

  pbuf_t *pb = pbuf_make(0, 0);
  if(pb == NULL)
    return;

  uint8_t *pkt = pbuf_data(pb, 0);
  pkt[0] = 0x20 | (signal >> 8);
  pkt[1] = signal;
  memcpy(pkt + 2, data, len);

  pb->pb_pktlen = len + 2;
  pb->pb_buflen = len + 2;

  mutex_lock(&dsig_emit_mutex);
  int empty = !STAILQ_FIRST(&dsig_emit_queue);
  STAILQ_INSERT_TAIL(&dsig_emit_queue, pb, pb_link);
  mutex_unlock(&dsig_emit_mutex);
  if(empty) {
    net_task_raise(&mbus_dsig_emit_task, 1);
  }
}
