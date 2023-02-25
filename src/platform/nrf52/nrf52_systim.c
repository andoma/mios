
#include <assert.h>
#include <sys/queue.h>
#include <stdint.h>
#include <stdio.h>
#include <unistd.h>
#include <mios/mios.h>
#include <mios/timer.h>
#include <mios/cli.h>

#include "irq.h"
#include "cpu.h"

#include "systick.h"

#include "nrf52_reg.h"
#include "nrf52_timer.h"

static struct timer_list systim_timers;

static void
systim_rearm(timer_t *t, int64_t now)
{
  const int64_t delta = t->t_expire - now;
  uint32_t delta32;
  if(delta < 2) {
    delta32 = 2;
  } else if(delta > 0xffffffffu) {
    delta32 = 0xffffffff;
  } else {
    delta32 = delta;
  }

  reg_wr(TIMER1_BASE + TIMER_TASKS_CLEAR, 1);
  reg_wr(TIMER1_BASE + TIMER_CC(0), delta32);
  reg_wr(TIMER1_BASE + TIMER_TASKS_START, 1);
}


void
irq_9(void)
{
  if(reg_rd(TIMER1_BASE + TIMER_EVENTS_COMPARE(0))) {
    reg_wr(TIMER1_BASE + TIMER_EVENTS_COMPARE(0), 0);

    const int64_t now = clock_get_irq_blocked();

    while(1) {
      timer_t *t = LIST_FIRST(&systim_timers);
      if(t == NULL)
        break;

      if(t->t_expire > now) {
        systim_rearm(t, now);
        break;
      }

      uint64_t expire = t->t_expire;
      LIST_REMOVE(t, t_link);
      t->t_expire = 0;
      t->t_cb(t->t_opaque, expire);
    }
  }
}


static int
systim_cmp(const timer_t *a, const timer_t *b)
{
  return a->t_expire > b->t_expire;
}


void
timer_arm_abs(timer_t *t, uint64_t deadline)
{
  timer_disarm(t);

  t->t_expire = deadline;
  LIST_INSERT_SORTED(&systim_timers, t, t_link, systim_cmp);
  if(t == LIST_FIRST(&systim_timers))
    systim_rearm(t, clock_get_irq_blocked());
}



static void  __attribute__((constructor(121)))
nrf52_systim_init(void)
{
  reg_wr(TIMER1_BASE + TIMER_BITMODE, 3);        // 32 bit width
  reg_wr(TIMER1_BASE + TIMER_INTENSET, 1 << 16); // Compare0 -> IRQ
  irq_enable(TIMER1_IRQ, IRQ_LEVEL_CLOCK);
}
