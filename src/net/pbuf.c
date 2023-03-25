#include <assert.h>
#include <malloc.h>
#include <string.h>
#include <stdio.h>
#include <mios/task.h>
#include <sys/queue.h>
#include <sys/param.h>

#include "irq.h"
#include "pbuf.h"


typedef struct pbuf_item {
  SLIST_ENTRY(pbuf_item) pi_link;
} pbuf_item_t;

SLIST_HEAD(pbuf_list, pbuf_item);

typedef struct pbuf_pool {
  struct pbuf_list pp_items;
  task_waitable_t pp_wait;
  int pp_avail;
} pbuf_pool_t;

static struct pbuf_pool pbuf_datas = { . pp_wait = WAITABLE_INITIALIZER("pbufdata")};
static struct pbuf_pool pbufs  = { . pp_wait = WAITABLE_INITIALIZER("pbuf")};

void net_buffers_available(void);

static void
pbuf_pool_put(pbuf_pool_t *pp, void *item)
{
  pbuf_item_t *pi = item;
  assert(((uint32_t)item & 0x3) == 0);
  SLIST_INSERT_HEAD(&pp->pp_items, pi, pi_link);

  if(pp->pp_avail < 2)
    task_wakeup(&pp->pp_wait, 0);
  if(pp->pp_avail == 0) {
    net_buffers_available();
  }
  pp->pp_avail++;
}

static void *
pbuf_pool_get(pbuf_pool_t *pp, int wait)
{
  if(!wait) {
    pbuf_item_t *pi = SLIST_FIRST(&pp->pp_items);
    if(pi != NULL) {
      SLIST_REMOVE_HEAD(&pp->pp_items, pi_link);
      pp->pp_avail--;
    }
    return pi;
  }

  while(pp->pp_avail < 2) {
    task_sleep(&pp->pp_wait);
  }

  pbuf_item_t *pi = SLIST_FIRST(&pp->pp_items);
  SLIST_REMOVE_HEAD(&pp->pp_items, pi_link);
  pp->pp_avail--;
  return pi;
}


static size_t
pbuf_pool_add(pbuf_pool_t *pp, void *start, void *end, size_t item_size)
{
  memset(start, 0, end - start);
  size_t count = 0;
  while(start + item_size <= end) {
    pbuf_pool_put(pp, start);
    start += item_size;
    count++;
  }
  return count;
}


void
pbuf_data_add(void *start, void *end)
{
  if(end == NULL) {
    if(pbuf_datas.pp_avail)
      return;
    const size_t size = PBUF_DATA_SIZE * 8;
    start = xalloc(size, 0, 0);
    end = start + size;
  }
  size_t count = pbuf_pool_add(&pbuf_datas, start, end, PBUF_DATA_SIZE);
  pbuf_alloc(count);
}

void *
pbuf_data_get(int wait)
{
  return pbuf_pool_get(&pbuf_datas, wait);
}

void
pbuf_data_put(void *buf)
{
  pbuf_pool_put(&pbuf_datas, buf);
}

void
pbuf_alloc(size_t count)
{
  void *start = xalloc(count * sizeof(pbuf_t), 0, 0);
  void *end = start + count * sizeof(pbuf_t);
  pbuf_pool_add(&pbufs, start, end, sizeof(pbuf_t));
}

pbuf_t *
pbuf_get(int wait)
{
  return pbuf_pool_get(&pbufs, wait);
}

void
pbuf_put(pbuf_t *pb)
{
  pbuf_pool_put(&pbufs, pb);
}


void
pbuf_free_irq_blocked(pbuf_t *pb)
{
  pbuf_t *next;
  for(; pb ; pb = next) {
    next = pb->pb_next;
    pbuf_data_put(pb->pb_data);
    pbuf_put(pb);
  }
}

pbuf_t *
pbuf_splice(struct pbuf_queue *pq)
{
  pbuf_t *pb = STAILQ_FIRST(pq);
  if(pb == NULL)
    return NULL;

  pbuf_t *last = pb;

  while(!(last->pb_flags & PBUF_EOP)) {
    last = STAILQ_NEXT(last, pb_link);
    if(last == NULL)
      break;
  }

  if(last != NULL) {
    STAILQ_REMOVE_HEAD_UNTIL(pq, last, pb_link);
    last->pb_next = NULL;
  }
  return pb;
}


pbuf_t *
pbuf_read(pbuf_t *pb, void *ptr, size_t len)
{
  assert(len <= pb->pb_pktlen);

  while(len) {
    size_t to_copy = MIN(len, pb->pb_buflen);

    memcpy(ptr, pb->pb_data + pb->pb_offset, to_copy);

    len           -= to_copy;
    pb->pb_offset += to_copy;
    pb->pb_buflen -= to_copy;
    pb->pb_pktlen -= to_copy;

    if(pb->pb_buflen == 0) {
      pbuf_t *n = pb->pb_next;
      n->pb_pktlen = pb->pb_pktlen;
      pbuf_data_put(pb->pb_data);
      pbuf_put(pb);
      pb = n;
    }
  }
  return pb;
}

pbuf_t *
pbuf_write(pbuf_t *head, const void *data, size_t len, size_t max_fill)
{
  if(head== NULL)
    return NULL;

  pbuf_t *pb = head;

  // Jump to end of chain
  while(pb->pb_next) {
    pb = pb->pb_next;
  }

  while(len) {

    if(pb->pb_buflen >= max_fill) {

      pb->pb_flags &= ~PBUF_EOP;

      pbuf_t *n = pbuf_get(0);
      if(n != NULL) {
        n->pb_next = NULL;
        n->pb_data = pbuf_data_get(0);
        if(n->pb_data == NULL) {
          pbuf_put(n);
          n = NULL;
        } else {
          n->pb_flags = PBUF_EOP;
          n->pb_pktlen = 0;
          n->pb_offset = 0;
          n->pb_buflen = 0;
        }
      }
      if(n == NULL) {
        pbuf_free(head);
        return NULL;
      }
      pb->pb_next = n;
      pb = n;
    }

    size_t to_copy = MIN(len, max_fill - pb->pb_buflen - pb->pb_offset);
    memcpy(pb->pb_data + pb->pb_offset + pb->pb_buflen, data, to_copy);

    head->pb_pktlen += to_copy;
    pb->pb_buflen += to_copy;
    len -= to_copy;
    data += to_copy;
  }
  return head;
}



pbuf_t *
pbuf_drop(pbuf_t *pb, size_t bytes)
{
  while(pb) {
    assert(bytes <= pb->pb_buflen); // Fix this case

    pb->pb_offset += bytes;
    pb->pb_buflen -= bytes;
    pb->pb_pktlen -= bytes;
    break;
  }
  return pb;
}


pbuf_t *
pbuf_trim(pbuf_t *pb, size_t bytes)
{
  while(pb) {
    assert(bytes < pb->pb_buflen); // Fix this case
    pb->pb_buflen -= bytes;
    pb->pb_pktlen -= bytes;
    break;
  }
  return pb;
}



pbuf_t *
pbuf_prepend(pbuf_t *pb, size_t bytes)
{
  assert(bytes <= pb->pb_offset); // Fix this case

  pb->pb_offset -= bytes;
  pb->pb_buflen += bytes;
  pb->pb_pktlen += bytes;
  return pb;
}


void *
pbuf_append(pbuf_t *pb, size_t bytes)
{
  assert(pb->pb_offset + bytes <= PBUF_DATA_SIZE);
  void *r = pb->pb_data + pb->pb_offset + pb->pb_buflen;
  pb->pb_buflen += bytes;
  pb->pb_pktlen += bytes;
  return r;
}

void
pbuf_free(pbuf_t *pb)
{
  int q = irq_forbid(IRQ_LEVEL_NET);
  pbuf_free_irq_blocked(pb);
  irq_permit(q);
}

pbuf_t *
pbuf_pullup(pbuf_t *pb, size_t bytes)
{
  if(pb->pb_buflen >= bytes)
    return pb;

  pbuf_free(pb);
  return NULL;
}


void
pbuf_reset(pbuf_t *pb, size_t header_size, size_t len)
{
  if(pb->pb_next) {
    pbuf_free(pb->pb_next);
    pb->pb_next = NULL;
  }
  pb->pb_flags = PBUF_SOP | PBUF_EOP;
  pb->pb_offset = header_size;
  pb->pb_buflen = len;
  pb->pb_pktlen = len;
}

pbuf_t *
pbuf_make_irq_blocked(int offset, int wait)
{
  pbuf_t *pb = pbuf_get(wait);
  if(pb != NULL) {
    pb->pb_next = NULL;
    pb->pb_data = pbuf_data_get(wait);
    if(pb->pb_data == NULL) {
      pbuf_put(pb);
    } else {
      pb->pb_flags = PBUF_SOP | PBUF_EOP;
      pb->pb_pktlen = 0;
      pb->pb_offset = offset;
      pb->pb_buflen = 0;
      return pb;
    }
  }
  return NULL;
}


pbuf_t *
pbuf_make(int offset, int wait)
{
  int q = irq_forbid(IRQ_LEVEL_NET);
  pbuf_t *pb = pbuf_make_irq_blocked(offset, wait);
  irq_permit(q);
  return pb;
}


pbuf_t *
pbuf_copy(const pbuf_t *src, int wait)
{
  int q = irq_forbid(IRQ_LEVEL_NET);
  pbuf_t *dst = pbuf_get(wait);
  if(dst != NULL) {
    dst->pb_next = NULL;
    dst->pb_data = pbuf_data_get(wait);
    if(dst->pb_data == NULL) {
      pbuf_put(dst);
    } else {
      irq_permit(q);
      dst->pb_flags = src->pb_flags;
      dst->pb_pktlen = src->pb_pktlen;
      dst->pb_offset = src->pb_offset;
      dst->pb_buflen = src->pb_buflen;
      memcpy(dst->pb_data + dst->pb_offset, src->pb_data + dst->pb_offset,
             src->pb_buflen);
      return dst;
    }
  }
  irq_permit(q);
  return NULL;
}


void
pbuf_status(void)
{
  printf("pbuf: %d avail\n", pbufs.pp_avail);
  printf("pbuf_data: %d avail\n", pbuf_datas.pp_avail);
}



void
pbuf_print(const char *prefix, const pbuf_t *pb, int full)
{
  for(; pb != NULL; pb = pb->pb_next) {

    printf("%s: %p %5d %c%c %p+%-4d [%4d]: ",
           prefix, pb,
           pb->pb_pktlen,
           pb->pb_flags & PBUF_SOP ? 'S' : ' ',
           pb->pb_flags & PBUF_EOP ? 'E' : ' ',
           pb->pb_data,
           pb->pb_offset,
           pb->pb_buflen);

    const uint8_t *data = pb->pb_data + pb->pb_offset;
    int tail;

    if(full) {
      tail = pb->pb_buflen;
    } else {

      int head = 4;
      tail = 4;

      if(pb->pb_buflen < 8) {
        tail = pb->pb_buflen / 2;
        head = pb->pb_buflen - tail;
      }

      for(int i = 0; i < head; i++) {
        printf("%02x ", data[i]);
      }

      if(pb->pb_buflen > 8) {
        printf("... ");
      }
    }
    for(int i = 0; i < tail; i++) {
      printf("%02x ", data[pb->pb_buflen - tail + i]);
    }

    printf("\n");
  }
}
