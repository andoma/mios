#include <assert.h>
#include <stdio.h>
#include <unistd.h>
#include <mios/task.h>

#include "irq.h"
#include "pbuf.h"

#include "netif.h"
#include "socket.h"

#define NET_WORK_PERIODIC  0

#define NET_WORK_NETIF_RX  8

_Static_assert(NET_WORK_NETIF_RX + NET_MAX_INTERFACES <= 32);


struct netif_list netifs;
mutex_t netif_mutex = MUTEX_INITIALIZER("netifs");

static task_waitable_t net_waitq = WAITABLE_INITIALIZER("net");
static uint32_t net_work_bits;
static timer_t net_periodic_timer;
static netif_t *interfaces[NET_MAX_INTERFACES];

void
netif_attach(netif_t *ni)
{
  STAILQ_INIT(&ni->ni_rx_queue);

  mutex_lock(&netif_mutex);

  for(int i = 0; i < NET_MAX_INTERFACES; i++) {
    if(interfaces[i] == NULL) {
      ni->ni_ifindex = i;
      interfaces[i] = ni;
      SLIST_INSERT_HEAD(&netifs, ni, ni_global_link);
      mutex_unlock(&netif_mutex);
      return;
    }
  }

  panic("ifindex range depleted");
}


void
netif_wakeup(netif_t *ni)
{
  net_work_bits |= (1 << (NET_WORK_NETIF_RX + ni->ni_ifindex));
  task_wakeup(&net_waitq, 0);
}



static void
net_periodic(void)
{
  netif_t *ni;

  mutex_lock(&netif_mutex);
  SLIST_FOREACH(ni, &netifs, ni_global_link) {
    ni->ni_periodic(ni);
  }
  mutex_unlock(&netif_mutex);
}


static void __attribute__((noreturn))
net_thread(void *arg)
{
  timer_arm_abs(&net_periodic_timer, clock_get() + 1000000, 0);

  int q = irq_forbid(IRQ_LEVEL_NET);

  while(1) {

    int bits = net_work_bits;
    if(bits == 0) {
      task_sleep(&net_waitq);
      continue;
    }

    net_work_bits = 0;
    while(bits) {
      int which = 31 - __builtin_clz(bits);
      if(which == NET_WORK_PERIODIC) {
        irq_permit(q);
        net_periodic();
        q = irq_forbid(IRQ_LEVEL_NET);

      } else if(which >= 8) {
        int ifindex = which - NET_WORK_NETIF_RX;
        netif_t *ni = interfaces[ifindex];
        pbuf_t *pb = pbuf_splice(&ni->ni_rx_queue);
        if(pb != NULL) {
          irq_permit(q);
          pb = ni->ni_input(ni, pb);
          q = irq_forbid(IRQ_LEVEL_NET);
          if(pb)
            pbuf_free_irq_blocked(pb);
        }
      }
      bits &= ~(1 << which);
    }
  }
}


static void
periodic_timer_cb(void *opaque, uint64_t expire)
{
  int q = irq_forbid(IRQ_LEVEL_NET);
  net_work_bits |= (1 << NET_WORK_PERIODIC);
  task_wakeup(&net_waitq, 0);
  irq_permit(q);

  timer_arm_abs(&net_periodic_timer, expire + 1000000, 0);
}


static void  __attribute__((constructor(190)))
net_main_init(void)
{
  pbuf_alloc(64);

  task_create((void *)net_thread, NULL, 512, "net", 0, 4);

  net_periodic_timer.t_cb = periodic_timer_cb;
}

