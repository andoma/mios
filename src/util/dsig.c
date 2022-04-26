#include <mios/dsig.h>
#include <sys/queue.h>
#include <net/socket.h>
#include <stdlib.h>
#include <unistd.h>

#include "net/mbus/mbus.h"
#include "irq.h"

static SLIST_HEAD(, dsig_sub) dsig_subs;

struct dsig_sub {
  SLIST_ENTRY(dsig_sub) ds_link;
  void (*ds_cb)(void *opaque, const void *data, size_t len);
  void *ds_opaque;
  uint8_t ds_signal;
  timer_t ds_timer;
};

static void
sub_timeout(void *opaque, uint64_t expire)
{
  dsig_sub_t *ds = opaque;
  ds->ds_cb(ds->ds_opaque, NULL, 0);
}


void
dsig_emit(uint8_t signal, const void *data, size_t len,
          uint8_t ttl, int flags)
{
  if(flags & DSIG_EMIT_LOCAL) {
    dsig_sub_t *ds;
    int q = irq_forbid(IRQ_LEVEL_CLOCK);
    SLIST_FOREACH(ds, &dsig_subs, ds_link) {
      if(ds->ds_signal == signal) {
        int64_t deadline = clock_get_irq_blocked() + ttl * 100000;
        timer_arm_abs(&ds->ds_timer, deadline);
        ds->ds_cb(ds->ds_opaque, data, len);
      }
    }
    irq_permit(q);
  }
#ifdef ENABLE_NET_MBUS
  if(flags & DSIG_EMIT_MBUS) {
    static socket_t *sock;
    static mutex_t mutex = MUTEX_INITIALIZER("dsig");
    mutex_lock(&mutex);
    if(sock == NULL) {
      sock = calloc(1, sizeof(socket_t));
      socket_init(sock, AF_MBUS, 0);

      error_t err = socket_attach(sock);
      if(err)
        panic("Unable to register DISG socket: %d", err);
    }
    mutex_unlock(&mutex);

    uint8_t hdr[3] = {MBUS_OP_DSIG_EMIT, signal, ttl};
    struct iovec iov[3];

    iov[0].iov_base = hdr;
    iov[0].iov_len = sizeof(hdr);

    iov[1].iov_base = (void *)data;
    iov[1].iov_len = len;

    socket_sendv(sock, iov, 2, SOCK_SEND_NONBLOCK | SOCK_SEND_BROADCAST);
  }
#endif
}


dsig_sub_t *
dsig_sub(uint8_t signal,
         void (*cb)(void *opaque, const void *data, size_t len),
         void *opaque)
{
  dsig_sub_t *ds = calloc(1, sizeof(dsig_sub_t));

  ds->ds_cb = cb;
  ds->ds_opaque = opaque;
  ds->ds_signal = signal;
  ds->ds_timer.t_cb = sub_timeout;
  ds->ds_timer.t_opaque = ds;
  int q = irq_forbid(IRQ_LEVEL_SWITCH);
  SLIST_INSERT_HEAD(&dsig_subs, ds, ds_link);
  irq_permit(q);
  return ds;
}
