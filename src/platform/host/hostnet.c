#include <string.h>
#include <sys/param.h>

#include <net/pbuf.h>
#include <net/ether.h>

#include "hostnet.h"

void
host_ether_rx(ether_netif_t *eni, const uint8_t *frame, size_t len)
{
  struct pbuf_queue pbq;
  STAILQ_INIT(&pbq);

  // Leave 2 bytes so the IP header after the 14 byte Ethernet header
  // ends up 4-byte aligned, same as the hardware drivers do.
  size_t offset = 2;
  size_t pos = 0;
  int first = 1;

  while(pos < len) {
    pbuf_t *pb = pbuf_get(0);
    void *buf = pb ? pbuf_data_get(0) : NULL;
    if(buf == NULL) {
      if(pb)
        pbuf_put(pb);
      pbuf_free_queue_irq_blocked(&pbq);
      eni->eni_stats.rx_sw_qdrop++;
      return;
    }
    const size_t chunk = MIN(len - pos, PBUF_DATA_SIZE - offset);
    memcpy(buf + offset, frame + pos, chunk);
    pb->pb_data = buf;
    pb->pb_offset = offset;
    pb->pb_buflen = chunk;
    pb->pb_flags = first ? PBUF_SOP : 0;
    pb->pb_pktlen = len;
    STAILQ_INSERT_TAIL(&pbq, pb, pb_link);
    pos += chunk;
    offset = 0;
    first = 0;
  }
  STAILQ_LAST(&pbq, pbuf, pb_link)->pb_flags |= PBUF_EOP;

  eni->eni_stats.rx_pkt++;
  eni->eni_stats.rx_byte += len;
  STAILQ_CONCAT(&eni->eni_ni.ni_rx_queue, &pbq);
}
