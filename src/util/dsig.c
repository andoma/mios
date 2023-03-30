#include <mios/dsig.h>
#include <mios/timer.h>
#include <sys/queue.h>
#include <net/socket.h>
#include <stdlib.h>
#include <unistd.h>

#ifdef ENABLE_NET_MBUS
#include "net/mbus/mbus_dsig.h"
#endif

#include "irq.h"

static SLIST_HEAD(, dsig_sub) dsig_subs;

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


#include <stdio.h>

void
dsig_dispatch(uint16_t signal, const void *data, size_t len)
{
  dsig_sub_t *ds;
  int q = irq_forbid(IRQ_LEVEL_CLOCK);
  uint64_t now = 0;
  SLIST_FOREACH(ds, &dsig_subs, ds_link) {
    if(ds->ds_signal == -1) {
      ds->ds_cbg(ds->ds_opaque, data, len, signal);
    } else if(ds->ds_signal == signal) {
      if(!now)
        now = clock_get_irq_blocked();
      timer_arm_abs(&ds->ds_timer, now + ds->ds_ttl * 1000);
      ds->ds_cb(ds->ds_opaque, data, len);
    }
  }
  irq_permit(q);
}



void
dsig_emit(uint16_t signal, const void *data, size_t len, int flags)
{
  if(flags & DSIG_EMIT_LOCAL) {
    dsig_dispatch(signal, data, len);
  }

#ifdef ENABLE_NET_MBUS
  if(flags & DSIG_EMIT_MBUS) {
    mbus_dsig_emit(signal, data, len);
  }
#endif
}


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

  int q = irq_forbid(IRQ_LEVEL_CLOCK);
  timer_arm_abs(&ds->ds_timer, clock_get_irq_blocked() + ttl * 1000);
  SLIST_INSERT_HEAD(&dsig_subs, ds, ds_link);
  irq_permit(q);

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
  int q = irq_forbid(IRQ_LEVEL_SWITCH);
  SLIST_INSERT_HEAD(&dsig_subs, ds, ds_link);
  irq_permit(q);
  return ds;
}
