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
  if(pbuf_pullup(pb, pb->pb_pktlen))
    return pb;
  // -6 accounts for header and trailing CRC
  dsig_dispatch(signal, pbuf_cdata(pb, 2), pb->pb_pktlen - 6);
  return pb;
}

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

  mbus_send(pb);
}
