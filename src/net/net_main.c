#include <assert.h>
#include <stdio.h>
#include <unistd.h>

#include <mios/task.h>
#include <mios/timer.h>

#include "irq.h"
#include "pbuf.h"

#include "netif.h"
#include "socket.h"

#define NET_WORK_PERIODIC  0
#define NET_WORK_SOCKET_OP 1
#define NET_WORK_SOCKET_TX 2
#define NET_WORK_BUFFERS_AVAIL 3
#define NET_WORK_NETIF_RX  8

_Static_assert(NET_WORK_NETIF_RX + NET_MAX_INTERFACES <= 32);

struct netif_list netifs;
mutex_t netif_mutex = MUTEX_INITIALIZER("netifs");

static task_waitable_t net_waitq = WAITABLE_INITIALIZER("net");
static uint32_t net_work_bits;
static timer_t net_periodic_timer;
static netif_t *interfaces[NET_MAX_INTERFACES];


struct socket_list sockets;
static struct socket_queue socket_op_queue =
  STAILQ_HEAD_INITIALIZER(socket_op_queue);

static void net_init(void);

static char net_initialized;

static void
net_wakeup(int bit)
{
  net_work_bits |= 1 << bit;
  task_wakeup(&net_waitq, 0);
}


void
net_wakeup_socket(socket_t *s)
{
  if(STAILQ_FIRST(&s->s_op_queue) == NULL &&
     STAILQ_FIRST(&s->s_tx_queue) == NULL) {
    if(STAILQ_FIRST(&socket_op_queue) == NULL)
      net_wakeup(NET_WORK_SOCKET_OP);
    STAILQ_INSERT_TAIL(&socket_op_queue, s, s_work_link);
  }
}

static error_t
socket_issue_ctl(socket_t *s, socket_ctl_op_t op)
{
  socket_ctl_t sc;

  task_waitable_init(&sc.sc_waitq, "sockop");
  sc.sc_op = op;

  int q = irq_forbid(IRQ_LEVEL_NET);
  net_wakeup_socket(s);
  STAILQ_INSERT_TAIL(&s->s_op_queue, &sc, sc_link);

  while(sc.sc_op)
    task_sleep(&sc.sc_waitq);
  irq_permit(q);
  return sc.sc_result;
}



error_t
socket_attach(socket_t *s)
{
  if(!__atomic_test_and_set(&net_initialized, __ATOMIC_SEQ_CST))
    net_init();

  return socket_issue_ctl(s, SOCKET_CTL_ATTACH);
}

error_t
socket_detach(socket_t *s)
{
  return socket_issue_ctl(s, SOCKET_CTL_DETACH);
}




void
netif_attach(netif_t *ni, const char *name, const device_class_t *dc)
{
  if(!__atomic_test_and_set(&net_initialized, __ATOMIC_SEQ_CST))
    net_init();

  STAILQ_INIT(&ni->ni_rx_queue);

  mutex_lock(&netif_mutex);

  for(int i = 0; i < NET_MAX_INTERFACES; i++) {
    if(interfaces[i] == NULL) {
      ni->ni_ifindex = i;
      interfaces[i] = ni;
      SLIST_INSERT_HEAD(&netifs, ni, ni_global_link);
      if(ni->ni_buffers_avail)
        ni->ni_buffers_avail(ni);
      ni->ni_dev.d_class = dc;
      ni->ni_dev.d_name = name;
      device_register(&ni->ni_dev);
      mutex_unlock(&netif_mutex);
      return;
    }
  }

  panic("ifindex range depleted");
}




static void
net_periodic(void)
{
  netif_t *ni;

  mutex_lock(&netif_mutex);
  SLIST_FOREACH(ni, &netifs, ni_global_link) {
    if(ni->ni_periodic != NULL)
      ni->ni_periodic(ni);
  }
  mutex_unlock(&netif_mutex);
}


static void
net_call_buffers_avail(void)
{
  netif_t *ni;

  mutex_lock(&netif_mutex);
  SLIST_FOREACH(ni, &netifs, ni_global_link) {
    if(ni->ni_buffers_avail != NULL)
      ni->ni_buffers_avail(ni);
  }
  mutex_unlock(&netif_mutex);
}


static void *__attribute__((noreturn))
net_thread(void *arg)
{
  int q = irq_forbid(IRQ_LEVEL_NET);

  timer_arm_abs(&net_periodic_timer, clock_get() + 1000000);

  while(1) {

    int bits = net_work_bits;
    if(bits == 0) {
      task_sleep(&net_waitq);
      continue;
    }

    net_work_bits = 0;
    while(bits) {
      int which = 31 - __builtin_clz(bits);

      if(unlikely(which == NET_WORK_PERIODIC)) {

        irq_permit(q);
        net_periodic();
        q = irq_forbid(IRQ_LEVEL_NET);

      } else if(which == NET_WORK_SOCKET_OP) {

        while(1) {
          socket_t *s = STAILQ_FIRST(&socket_op_queue);
          if(s == NULL)
            break;

          pbuf_t *pb = pbuf_splice(&s->s_tx_queue);
          if(pb != NULL) {

            if(STAILQ_FIRST(&s->s_op_queue) == NULL &&
               STAILQ_FIRST(&s->s_tx_queue) == NULL)
              STAILQ_REMOVE_HEAD(&socket_op_queue, s_work_link);

            irq_permit(q);
            pb = s->s_proto->sp_xmit_pbuf(s, pb);
            q = irq_forbid(IRQ_LEVEL_NET);
            if(pb)
              pbuf_free_irq_blocked(pb);
            continue;
          }

          socket_ctl_t *sc = STAILQ_FIRST(&s->s_op_queue);
          assert(sc != NULL);
          STAILQ_REMOVE_HEAD(&s->s_op_queue, sc_link);

          if(STAILQ_FIRST(&s->s_op_queue) == NULL)
            STAILQ_REMOVE_HEAD(&socket_op_queue, s_work_link);

          irq_permit(q);
          sc->sc_result = socket_net_ctl(s, sc);
          q = irq_forbid(IRQ_LEVEL_NET);
          sc->sc_op = 0;
          task_wakeup(&sc->sc_waitq, 0);
        }

      } else if(which >= NET_WORK_NETIF_RX) {

        int ifindex = which - NET_WORK_NETIF_RX;
        netif_t *ni = interfaces[ifindex];
        pbuf_t *pb = pbuf_splice(&ni->ni_rx_queue);
        if(pb != NULL) {
          if(STAILQ_FIRST(&ni->ni_rx_queue) != NULL)
            net_work_bits |= (1 << which);

          irq_permit(q);
          pb = ni->ni_input(ni, pb);
          q = irq_forbid(IRQ_LEVEL_NET);
          if(pb)
            pbuf_free_irq_blocked(pb);
        }

      } else if(unlikely(which == NET_WORK_BUFFERS_AVAIL)) {

        irq_permit(q);
        net_call_buffers_avail();
        q = irq_forbid(IRQ_LEVEL_NET);

      }

      bits &= ~(1 << which);
    }
  }
}


void
netif_wakeup(netif_t *ni)
{
  net_wakeup(NET_WORK_NETIF_RX + ni->ni_ifindex);
}


void
net_buffers_available(void)
{
  net_wakeup(NET_WORK_BUFFERS_AVAIL);
}


static void
periodic_timer_cb(void *opaque, uint64_t expire)
{
  int q = irq_forbid(IRQ_LEVEL_NET);
  net_wakeup(NET_WORK_PERIODIC);
  irq_permit(q);

  timer_arm_abs(&net_periodic_timer, expire + 1000000);
}


static void
net_init(void)
{
  int q = irq_forbid(IRQ_LEVEL_NET);
  // If no pbufs has been allocated by platform specific code,
  // we allocate some now
  pbuf_data_add(NULL, NULL);
  irq_permit(q);

  int flags = 0;
#ifdef ENABLE_NET_FPU_USAGE
  flags |= TASK_FPU;
#endif

  thread_create(net_thread, NULL, 768, "net", flags, 5);
  net_periodic_timer.t_cb = periodic_timer_cb;
}

