#include <mios/dsig.h>
#include <mios/timer.h>
#include <mios/task.h>
#include <sys/queue.h>
#include <stdlib.h>
#include <unistd.h>

#ifdef ENABLE_NET_MBUS
#include "net/mbus/mbus_dsig.h"
#endif

#include "net/net_task.h"

static SLIST_HEAD(, dsig_sub) dsig_subs;
static SLIST_HEAD(, dsig_sub) dsig_pending_subs;


static mutex_t dsig_sub_mutex = MUTEX_INITIALIZER("dsigsub");

struct dsig_sub {
  SLIST_ENTRY(dsig_sub) ds_link;
  union {
    void (*ds_cb)(void *opaque, const void *data, size_t len);
    void (*ds_cbg)(void *opaque, const void *data, size_t len,
                   uint16_t signal);
  };
  void *ds_opaque;
  timer_t ds_timer;
  int16_t ds_signal;
  uint16_t ds_ttl;
};

static void
sub_timeout(void *opaque, uint64_t expire)
{
  dsig_sub_t *ds = opaque;
  ds->ds_cb(ds->ds_opaque, NULL, 0);
}


void
dsig_dispatch(uint16_t signal, const void *data, size_t len)
{
  uint64_t now = 0;
  dsig_sub_t *ds;

  mutex_lock(&dsig_sub_mutex);
  SLIST_FOREACH(ds, &dsig_subs, ds_link) {
    if(ds->ds_signal == -1) {
      ds->ds_cbg(ds->ds_opaque, data, len, signal);
    } else if(ds->ds_signal == signal) {
      if(!now)
        now = clock_get();
      net_timer_arm(&ds->ds_timer, now + ds->ds_ttl * 1000);
      ds->ds_cb(ds->ds_opaque, data, len);
    }
  }
  mutex_unlock(&dsig_sub_mutex);
}



void
dsig_emit(uint16_t signal, const void *data, size_t len)
{
#ifdef ENABLE_NET_MBUS
  mbus_dsig_emit(signal, data, len);
#endif
}


static void
dsig_sub_insert_cb(net_task_t *nt, uint32_t signals)
{
  mutex_lock(&dsig_sub_mutex);
  while(1) {
    dsig_sub_t *ds = SLIST_FIRST(&dsig_pending_subs);
    if(ds != NULL)
      SLIST_REMOVE_HEAD(&dsig_pending_subs, ds_link);

    if(ds == NULL)
      break;

    if(ds->ds_ttl)
      net_timer_arm(&ds->ds_timer, clock_get() + ds->ds_ttl * 1000);

    SLIST_INSERT_HEAD(&dsig_subs, ds, ds_link);
  }
  mutex_unlock(&dsig_sub_mutex);
}


static net_task_t dsig_sub_insert_task = { dsig_sub_insert_cb };


dsig_sub_t *
dsig_sub(uint16_t signal, uint16_t ttl,
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
                        uint16_t signal),
             void *opaque)
{
  dsig_sub_t *ds = calloc(1, sizeof(dsig_sub_t));
  ds->ds_cbg = cb;
  ds->ds_opaque = opaque;
  ds->ds_signal = -1;

  mutex_lock(&dsig_sub_mutex);
  SLIST_INSERT_HEAD(&dsig_pending_subs, ds, ds_link);
  mutex_unlock(&dsig_sub_mutex);
  net_task_raise(&dsig_sub_insert_task, 1);

  return ds;
}
