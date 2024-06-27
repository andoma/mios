#include "dsig.h"

#include <mios/dsig.h>
#include <mios/timer.h>
#include <mios/task.h>
#include <mios/bytestream.h>
#include <sys/queue.h>
#include <stdlib.h>
#include <unistd.h>

#include "net/net_task.h"
#include "net/pbuf.h"
#include "net/netif.h"

#include <stdio.h>

static SLIST_HEAD(, dsig_sub) dsig_subs;
static SLIST_HEAD(, dsig_sub) dsig_pending_subs;

static mutex_t dsig_sub_mutex = MUTEX_INITIALIZER("dsigsub");

struct dsig_sub {
  SLIST_ENTRY(dsig_sub) ds_link;
  union {
    void (*ds_cb)(void *opaque, const void *data, size_t len);
    void (*ds_cbg)(void *opaque, const void *data, size_t len,
                   uint32_t signal);
  };
  void *ds_opaque;
  timer_t ds_timer;
  uint32_t ds_signal;
  uint16_t ds_ttl;
};

static void
sub_timeout(void *opaque, uint64_t expire)
{
  dsig_sub_t *ds = opaque;
  ds->ds_cb(ds->ds_opaque, NULL, 0);
}

void
dsig_dispatch(uint32_t signal, const void *data, size_t len)
{
  uint64_t now = 0;
  dsig_sub_t *ds;

  SLIST_FOREACH(ds, &dsig_subs, ds_link) {
    if(ds->ds_signal == UINT32_MAX) {
      ds->ds_cbg(ds->ds_opaque, data, len, signal);
    } else if(ds->ds_signal == signal) {
      if(!now)
        now = clock_get();
      net_timer_arm(&ds->ds_timer, now + ds->ds_ttl * 1000);
      ds->ds_cb(ds->ds_opaque, data, len);
    }
  }
}

static void
dsig_output_iface(uint32_t id, struct pbuf **pb, struct netif *ni)
{
  const struct dsig_output_filter *dof = ni->ni_dsig_output_filter;
  uint32_t flags = 0;

  if(dof) {
    int hit = 0;
    while(dof->start != UINT32_MAX) {
      if(id >= dof->start && id < dof->end) {

        if(dof->flags & DSIG_FILTER_INCLUDE)
          hit = 1;
        if(dof->flags & DSIG_FILTER_EXCLUDE)
          hit = 0;
      }
      dof++;
    }
    if(!hit)
      return;
  }

  pbuf_t *copy = pbuf_copy(*pb, 0);
  if(copy == NULL)
    return;

  copy = ni->ni_dsig_output(ni, copy, id, flags);
  if(copy != NULL)
    pbuf_free(copy);
}


// Called on net thread for packets to be sent
struct pbuf *
dsig_output(uint32_t id, struct pbuf *pb, struct netif *exclude)
{
  struct netif *ni;
  SLIST_FOREACH(ni, &netifs, ni_global_link) {
    if(ni == exclude || ni->ni_dsig_output == NULL)
      continue;
    dsig_output_iface(id, &pb, ni);
  }
  return pb;
}


// Called from network-driver/protocol input code
struct pbuf *
dsig_input(uint32_t id, struct pbuf *pb, struct netif *ni)
{
  if(pbuf_pullup(pb, pb->pb_pktlen))
    return pb;

  // Local dispatch
  dsig_dispatch(id, pbuf_cdata(pb, 0), pb->pb_pktlen);

  // Forwarding
  return dsig_output(id, pb, ni);
}




static mutex_t dsig_send_mutex = MUTEX_INITIALIZER("dsigsend");

static struct pbuf_queue dsig_send_queue =
  STAILQ_HEAD_INITIALIZER(dsig_send_queue);

static void
dsig_send_cb(net_task_t *nt, uint32_t signals)
{
  while(1) {
    mutex_lock(&dsig_send_mutex);
    pbuf_t *pb = pbuf_splice(&dsig_send_queue);
    mutex_unlock(&dsig_send_mutex);
    if(pb == NULL)
      break;

    uint32_t group = rd32_le(pbuf_cdata(pb, 0));
    pb = pbuf_drop(pb, 4); // Drop group
    pb = dsig_output(group, pb, NULL);
    if(pb != NULL)
      pbuf_free(pb);
  }
}

static net_task_t dsig_send_task = { dsig_send_cb };

static void
dsig_send(pbuf_t *pb)
{
  mutex_lock(&dsig_send_mutex);
  int empty = !STAILQ_FIRST(&dsig_send_queue);
  STAILQ_INSERT_TAIL(&dsig_send_queue, pb, pb_link);
  mutex_unlock(&dsig_send_mutex);
  if(empty) {
    net_task_raise(&dsig_send_task, 1);
  }
}


// Called from "user" code on this host
void
dsig_emit(uint32_t signal, const void *data, size_t len)
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

  dsig_send(pb);
}




static void
dsig_sub_insert_cb(net_task_t *nt, uint32_t signals)
{
  while(1) {
    mutex_lock(&dsig_sub_mutex);
    dsig_sub_t *ds = SLIST_FIRST(&dsig_pending_subs);
    if(ds != NULL)
      SLIST_REMOVE_HEAD(&dsig_pending_subs, ds_link);
    mutex_unlock(&dsig_sub_mutex);

    if(ds == NULL)
      break;

    if(ds->ds_ttl)
      net_timer_arm(&ds->ds_timer, clock_get() + ds->ds_ttl * 1000);

    SLIST_INSERT_HEAD(&dsig_subs, ds, ds_link);
  }
}


static net_task_t dsig_sub_insert_task = { dsig_sub_insert_cb };


dsig_sub_t *
dsig_sub(uint32_t signal, uint16_t ttl,
         void (*cb)(void *opaque, const void *data, size_t len),
         void *opaque)
{
  dsig_sub_t *ds = calloc(1, sizeof(dsig_sub_t));
  ds->ds_cb = cb;
  ds->ds_opaque = opaque;
  ds->ds_signal = signal;
  ds->ds_ttl = ttl;
  ds->ds_timer.t_cb = sub_timeout;
  ds->ds_timer.t_opaque = ds;

  mutex_lock(&dsig_sub_mutex);
  SLIST_INSERT_HEAD(&dsig_pending_subs, ds, ds_link);
  mutex_unlock(&dsig_sub_mutex);
  net_task_raise(&dsig_sub_insert_task, 1);
  return ds;
}



dsig_sub_t *
dsig_sub_all(void (*cb)(void *opaque, const void *data, size_t len,
                        uint32_t signal),
             void *opaque)
{
  dsig_sub_t *ds = calloc(1, sizeof(dsig_sub_t));
  ds->ds_cbg = cb;
  ds->ds_opaque = opaque;
  ds->ds_signal = UINT32_MAX;

  mutex_lock(&dsig_sub_mutex);
  SLIST_INSERT_HEAD(&dsig_pending_subs, ds, ds_link);
  mutex_unlock(&dsig_sub_mutex);
  net_task_raise(&dsig_sub_insert_task, 1);

  return ds;
}
