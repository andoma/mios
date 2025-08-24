#include <mios/stream.h>
#include <mios/pushpull.h>
#include <mios/task.h>
#include <mios/eventlog.h>
#include <mios/service.h>

#include <sys/param.h>

#include <string.h>
#include <malloc.h>
#include <assert.h>
#include <stdlib.h>

#include "irq.h"

#include "net/pbuf.h"

typedef struct pushpull_stream {

  stream_t pps_stream;

  pushpull_t *pps_pp;

  mutex_t pps_tx_mutex;
  cond_t pps_tx_cond;
  mutex_t pps_rx_mutex;
  cond_t pps_rx_cond;

  pbuf_t *pps_rxbuf;

  pbuf_t *pps_txbuf_head;
  pbuf_t *pps_txbuf_tail;

  uint8_t pps_shutdown;
  uint8_t pps_flushed;

} pushpull_stream_t;


static ssize_t
pushpull_stream_read(struct stream *s, void *buf, size_t size,
                     size_t requested)
{
  pushpull_stream_t *pps = (pushpull_stream_t *)s;

  int off = 0;
  mutex_lock(&pps->pps_rx_mutex);

  while(1) {

    if(pps->pps_shutdown)
      break;

    if(pps->pps_rxbuf == NULL) {
      if(off >= requested)
        break;
      pps->pps_pp->net->event(pps->pps_pp->net_opaque,
                                PUSHPULL_EVENT_PUSH);
      cond_wait(&pps->pps_rx_cond, &pps->pps_rx_mutex);
      continue;
    }

    const size_t avail = pps->pps_rxbuf->pb_pktlen;
    const size_t to_copy = MIN(avail, size - off);
    pps->pps_rxbuf = pbuf_read(pps->pps_rxbuf, buf + off, to_copy);

    off += to_copy;
    if(off == size)
      break;
  }
  mutex_unlock(&pps->pps_rx_mutex);
  return off;
}


ssize_t
pushpull_stream_write(struct stream *s, const void *buf, size_t size,
                      int flags)
{
  pushpull_stream_t *pps = (pushpull_stream_t *)s;
  size_t written = 0;
  mutex_lock(&pps->pps_tx_mutex);

  if(buf == NULL) {
    // Flush

    if(pps->pps_txbuf_head != NULL) {
      pps->pps_flushed = 1;
      pps->pps_pp->net->event(pps->pps_pp->net_opaque,
                                PUSHPULL_EVENT_PULL);
    }

  } else {

    while(size) {

      if(pps->pps_shutdown)
        break;

      if(pps->pps_txbuf_head == NULL) {
        pps->pps_txbuf_head = pbuf_make(pps->pps_pp->preferred_offset, 0);
        if(pps->pps_txbuf_head == NULL) {
          pps->pps_flushed = 1;
          pps->pps_pp->net->event(pps->pps_pp->net_opaque,
                                    PUSHPULL_EVENT_PULL);
          if(flags & STREAM_WRITE_NO_WAIT)
            break;
          pps->pps_txbuf_head = pbuf_make(pps->pps_pp->preferred_offset, 1);
        }
        pps->pps_txbuf_tail = pps->pps_txbuf_head;
      }

      size_t remain = pps->pps_pp->max_fragment_size -
        pps->pps_txbuf_head->pb_pktlen;

      // Check if we need to allocate a new buffer for filling
      if(remain &&
         pps->pps_txbuf_tail->pb_buflen +
         pps->pps_txbuf_tail->pb_offset == PBUF_DATA_SIZE) {

        pbuf_t *pb = pbuf_make(0, 0);
        if(pb == NULL) {
          // Can't alloc, flush what we have
          remain = 0;
        } else {
          pb->pb_flags &= ~PBUF_SOP;
          pps->pps_txbuf_tail->pb_flags &= ~PBUF_EOP;
          pps->pps_txbuf_tail->pb_next = pb;
          pps->pps_txbuf_tail = pb;
        }

      }

      if(remain == 0) {
        pps->pps_flushed = 1;
        pps->pps_pp->net->event(pps->pps_pp->net_opaque, PUSHPULL_EVENT_PULL);
        cond_wait(&pps->pps_tx_cond, &pps->pps_tx_mutex);
        continue;
      }
      pbuf_t *pb = pps->pps_txbuf_tail;
      size_t to_copy = MIN(MIN(size, remain),
                           PBUF_DATA_SIZE - (pb->pb_offset + pb->pb_buflen));

      memcpy(pb->pb_data + pb->pb_offset + pb->pb_buflen, buf, to_copy);
      pb->pb_buflen += to_copy;
      pps->pps_txbuf_head->pb_pktlen += to_copy;
      buf += to_copy;
      size -= to_copy;
      written += to_copy;
    }
  }
  mutex_unlock(&pps->pps_tx_mutex);
  return written;
}

static task_waitable_t *
pushpull_stream_poll(stream_t *s, poll_type_t type)
{
  pushpull_stream_t *pps = (pushpull_stream_t *)s;

  if(type == POLL_STREAM_WRITE) {
    mutex_lock(&pps->pps_tx_mutex);
    cond_t *w = (pps->pps_pp->max_fragment_size -
      pps->pps_txbuf_head->pb_pktlen) != 0 ?
      &pps->pps_tx_cond : NULL;
    irq_forbid(IRQ_LEVEL_SCHED);
    mutex_unlock(&pps->pps_tx_mutex);
    return w;
  } else {
    mutex_lock(&pps->pps_rx_mutex);
    cond_t *r = pps->pps_rxbuf == NULL ?
      &pps->pps_rx_cond : NULL;
    irq_forbid(IRQ_LEVEL_SCHED);
    mutex_unlock(&pps->pps_rx_mutex);
    return r;
  }

}


static void
pushpull_stream_wait_shutdown(pushpull_stream_t *pps)
{
  mutex_lock(&pps->pps_tx_mutex);
  while(!pps->pps_shutdown)
    cond_wait(&pps->pps_tx_cond, &pps->pps_tx_mutex);
  mutex_unlock(&pps->pps_tx_mutex);

  pbuf_free(pps->pps_rxbuf);
  pbuf_free(pps->pps_txbuf_head);
}


static struct pbuf *
pushpull_stream_pull(void *opaque)
{
  pushpull_stream_t *pps = opaque;
  pbuf_t *pb = NULL;
  mutex_lock(&pps->pps_tx_mutex);
  if(pps->pps_flushed) {
    pb = pps->pps_txbuf_head;
    if(pb != NULL) {
      pps->pps_txbuf_head = NULL;
      pps->pps_txbuf_tail = NULL;
      cond_signal(&pps->pps_tx_cond);
      pps->pps_flushed = 0;
    }
  }
  mutex_unlock(&pps->pps_tx_mutex);
  return pb;
}


static uint32_t
pushpull_stream_push(void *opaque, struct pbuf *pb)
{
  pushpull_stream_t *pps = opaque;

  mutex_lock(&pps->pps_rx_mutex);
  assert(pps->pps_rxbuf == NULL);
  pps->pps_rxbuf = pb;
  cond_signal(&pps->pps_rx_cond);
  mutex_unlock(&pps->pps_rx_mutex);
  return 0;
}

static int
pushpull_stream_may_push(void *opaque)
{
  pushpull_stream_t *pps = opaque;
  return pps->pps_rxbuf == NULL;
}


static void
pushpull_stream_close_pp(void *opaque, const char *reason)
{
  pushpull_stream_t *pps = opaque;
  mutex_lock(&pps->pps_tx_mutex);
  pps->pps_shutdown = 1;
  cond_signal(&pps->pps_tx_cond);
  mutex_unlock(&pps->pps_tx_mutex);
}


static void
pushpull_stream_destroy(stream_t *s)
{
  pushpull_stream_t *pps = (pushpull_stream_t *)s;
  pps->pps_pp->net->event(pps->pps_pp->net_opaque, PUSHPULL_EVENT_CLOSE);
  pushpull_stream_wait_shutdown(pps);
  free(pps);
}



static const pushpull_app_fn_t pps_pushpull_vtable = {
  .push = pushpull_stream_push,
  .may_push = pushpull_stream_may_push,
  .pull = pushpull_stream_pull,
  .close = pushpull_stream_close_pp,
};

static const stream_vtable_t pps_stream_vtable = {
  .read = pushpull_stream_read,
  .write = pushpull_stream_write,
  .poll = pushpull_stream_poll,
  .close = pushpull_stream_destroy,
};


error_t
service_open_pushpull(const service_t *svc, pushpull_t *pp)
{
  if(svc->open_pushpull != NULL) {
    return svc->open_pushpull(pp);
  }

  // Service does not support the pushpull interface (it's a stream service)
  // setup the bridge

  pushpull_stream_t *pps = xalloc(sizeof(pushpull_stream_t), 0,
                                  MEM_MAY_FAIL | MEM_CLEAR);
  if(pps == NULL)
    return ERR_NO_MEMORY;

  mutex_init(&pps->pps_tx_mutex, "tsocket");
  mutex_init(&pps->pps_rx_mutex, "rsocket");
  cond_init(&pps->pps_tx_cond, "tsocket");
  cond_init(&pps->pps_rx_cond, "rsocket");

  pps->pps_pp = pp;
  pps->pps_stream.vtable = &pps_stream_vtable;

  pp->app = &pps_pushpull_vtable;
  pp->app_opaque = pps;

  error_t err = svc->open_stream(&pps->pps_stream);
  if(err) {
    free(pps);
  }
  return err;
}
