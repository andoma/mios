#include "net_task.h"

#include "pbuf.h"
#include "irq.h"

#include <mios/eventlog.h>

#include <stdio.h>
#include <string.h>

static struct pbuf_queue netlog_queue =
  STAILQ_HEAD_INITIALIZER(netlog_queue);


static void
netlog_cb(struct net_task *nt, uint32_t signals)
{
  while(1) {
    int q = irq_forbid(IRQ_LEVEL_NET);
    pbuf_t *pb = pbuf_splice(&netlog_queue);
    irq_permit(q);
    if(pb == NULL)
      break;
    evlog(LOG_DEBUG, "%s", (const char *)pbuf_cdata(pb, 0));
    pbuf_free(pb);
  }
}


static net_task_t netlog_task = { netlog_cb };


void
netlog(const char *fmt, ...)
{
  va_list ap;

  pbuf_t *pb = pbuf_make_irq_blocked(0,0);
  if(pb == NULL)
    return;

  va_start(ap, fmt);
  pb->pb_pktlen = vsnprintf(pbuf_data(pb, 0), PBUF_DATA_SIZE, fmt, ap);
  va_end(ap);
  pb->pb_flags = PBUF_SOP | PBUF_EOP;
  pb->pb_buflen = pb->pb_pktlen;
  STAILQ_INSERT_TAIL(&netlog_queue, pb, pb_link);
  net_task_raise(&netlog_task, 1);
}


void
netlog_hexdump(const char *prefix, const uint8_t *buf, size_t len)
{
  const size_t prefixlen = strlen(prefix);
  while(len > 0) {

    pbuf_t *pb = pbuf_make_irq_blocked(0,0);
    if(pb == NULL)
      break;

    char *dst = pbuf_data(pb, 0);
    memcpy(dst, prefix, prefixlen);

    size_t off = prefixlen;
    dst[off++] = ':';
    dst[off++] = ' ';

    for(size_t i = 0; i < 16 && len > 0; i++) {
      uint8_t b = *buf++;
      dst[off++] = "0123456789abcdef"[b >> 4];
      dst[off++] = "0123456789abcdef"[b & 0xf];
      dst[off++] = ' ';
      len--;
    }
    dst[off] = 0;
    pb->pb_flags = PBUF_SOP | PBUF_EOP;
    pb->pb_buflen = off;
    STAILQ_INSERT_TAIL(&netlog_queue, pb, pb_link);
  }
  net_task_raise(&netlog_task, 1);
}

