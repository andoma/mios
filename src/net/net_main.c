#include <assert.h>
#include <stdio.h>
#include <unistd.h>
#include <string.h>

#include <mios/task.h>
#include <mios/timer.h>
#include <mios/eventlog.h>

#include "irq.h"
#include "pbuf.h"

#include "netif.h"
#include "net_task.h"

struct netif_list netifs;

static mutex_t netif_mutex = MUTEX_INITIALIZER("netifs");

static task_waitable_t net_waitq = WAITABLE_INITIALIZER("net");

STAILQ_HEAD(net_task_queue, net_task);

static struct net_task_queue net_tasks = STAILQ_HEAD_INITIALIZER(net_tasks);

static struct timer_list net_timers;

static void
netif_task_cb(net_task_t *nt, uint32_t signals)
{
  netif_t *ni = ((void *)nt) - offsetof(netif_t, ni_task);

  int q = irq_forbid(IRQ_LEVEL_NET);

  if(signals & NETIF_TASK_RX) {

    while(1) {
      pbuf_t *pb = pbuf_splice(&ni->ni_rx_queue);
      if(pb == NULL)
        break;
      irq_permit(q);
      pb = ni->ni_input(ni, pb);
      q = irq_forbid(IRQ_LEVEL_NET);
      if(pb)
        pbuf_free_irq_blocked(pb);
    }
  }

  if(signals & NETIF_TASK_STATUS_UP && !(ni->ni_flags & NETIF_F_UP)) {
    ni->ni_flags |= NETIF_F_UP;
    if(ni->ni_status_change != NULL)
      ni->ni_status_change(ni);
  }
  if(signals & NETIF_TASK_STATUS_DOWN && (ni->ni_flags & NETIF_F_UP)) {
    ni->ni_flags &= ~NETIF_F_UP;
    if(ni->ni_status_change)
      ni->ni_status_change(ni);
  }
  irq_permit(q);
}


static void *__attribute__((noreturn))
net_thread(void *arg)
{
  int q = irq_forbid(IRQ_LEVEL_NET);

  while(1) {
    net_task_t *nt = STAILQ_FIRST(&net_tasks);
    if(nt != NULL) {
      uint32_t signals = nt->nt_signals;
      nt->nt_signals = 0;
      STAILQ_REMOVE_HEAD(&net_tasks, nt_link);
      irq_permit(q);
      nt->nt_cb(nt, signals);
      q = irq_forbid(IRQ_LEVEL_NET);
      continue;
    }

    const timer_t *t = LIST_FIRST(&net_timers);
    if(t == NULL) {
      task_sleep(&net_waitq);
    } else if(task_sleep_deadline(&net_waitq, t->t_expire)) {
      uint64_t now = clock_get_irq_blocked();
      irq_permit(q);
      timer_dispatch(&net_timers, now);
      q = irq_forbid(IRQ_LEVEL_NET);
    }
  }
}


static void
netbuf_cb(net_task_t *nt, uint32_t signals)
{
  netif_t *ni;

  mutex_lock(&netif_mutex);
  SLIST_FOREACH(ni, &netifs, ni_global_link) {
    if(ni->ni_buffers_avail != NULL)
      ni->ni_buffers_avail(ni);
  }
  mutex_unlock(&netif_mutex);
}


static net_task_t netbuf_task = { netbuf_cb };

void
net_buffers_available(void)
{
  net_task_raise(&netbuf_task, 1);
}


void
net_timer_arm(timer_t *t, uint64_t deadline)
{
  timer_arm_on_queue(t, deadline, &net_timers);
}


void
net_task_raise(net_task_t *nt, uint32_t signals)
{
  assert(signals);
  int q = irq_forbid(IRQ_LEVEL_SCHED);

  if(nt->nt_signals == 0) {
    STAILQ_INSERT_TAIL(&net_tasks, nt, nt_link);
    task_wakeup_sched_locked(&net_waitq, 0);
  }
  nt->nt_signals |= signals;
  irq_permit(q);
}

void
netif_attach(netif_t *ni, const char *name, const device_class_t *dc)
{
  STAILQ_INIT(&ni->ni_rx_queue);

  ni->ni_task.nt_cb = netif_task_cb;

  mutex_lock(&netif_mutex);

  SLIST_INSERT_HEAD(&netifs, ni, ni_global_link);
  if(ni->ni_buffers_avail)
    ni->ni_buffers_avail(ni);
  ni->ni_dev.d_class = dc;
  ni->ni_dev.d_name = name;
  mutex_unlock(&netif_mutex);

  device_register(&ni->ni_dev);
}


static void
net_task_cancel(net_task_t *nt)
{
  if(nt->nt_signals) {
    STAILQ_REMOVE(&net_tasks, nt, net_task, nt_link);
    nt->nt_signals = 0;
  }
}


void
netif_detach(netif_t *ni)
{
  int q = irq_forbid(IRQ_LEVEL_SCHED);
  net_task_cancel(&ni->ni_task);
  pbuf_free_queue_irq_blocked(&ni->ni_rx_queue);
  irq_permit(q);

  mutex_lock(&netif_mutex);
  SLIST_REMOVE(&netifs, ni, netif, ni_global_link);
  mutex_unlock(&netif_mutex);

  device_unregister(&ni->ni_dev);
}


netif_t *
netif_get_net(netif_t *cur)
{
  netif_t *ni;

  mutex_lock(&netif_mutex);
  if(cur == NULL) {
    ni = SLIST_FIRST(&netifs);
  } else {
    SLIST_FOREACH(ni, &netifs, ni_global_link) {
      if(ni == cur) {
        break;
      }
    }
    if(ni)
      ni = SLIST_NEXT(ni, ni_global_link);
  }
  if(ni)
    device_retain(&ni->ni_dev);
  mutex_unlock(&netif_mutex);
  if(cur)
    device_release(&cur->ni_dev);
  return ni;
}


static void __attribute__((constructor(180)))
net_init(void)
{
  // If no pbufs has been allocated by platform specific code,
  // we allocate some now
  pbuf_data_add(NULL, NULL);

  int flags = 0;
#ifdef ENABLE_NET_FPU_USAGE
  flags |= TASK_FPU;
#endif
  thread_create(net_thread, NULL, 768, "net", flags, 10);
}
