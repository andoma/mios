
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
#include "nrf52_rtc.h"

static struct timer_list systim_rtc1_timers;

static void
systim_rtc1_rearm(timer_t *t, int64_t now)
{
  const int64_t delta = t->t_expire - now;
  uint32_t delta32;

  if(delta < 2) {
    delta32 = 2;
  } else if(delta > 0x1fffffff) {
    delta32 = 0xffffff;
  } else {
    // Convert from 1MHz to 32678Hz
    delta32 = (delta * (uint64_t)140738560) >> 32;
    delta32++;
    if(delta32 > 0xffffff)
      delta32 = 0xffffff;
  }

  reg_wr(RTC1_BASE + TIMER_TASKS_CLEAR, 1);
  reg_wr(RTC1_BASE + TIMER_CC(0), delta32);
  reg_wr(RTC1_BASE + TIMER_TASKS_START, 1);
}


void
irq_17(void)
{
  if(reg_rd(RTC1_BASE + TIMER_EVENTS_COMPARE(0))) {
    reg_wr(RTC1_BASE + TIMER_EVENTS_COMPARE(0), 0);

    const int64_t now = clock_get_irq_blocked();

    while(1) {
      timer_t *t = LIST_FIRST(&systim_rtc1_timers);
      if(t == NULL)
        break;

      if(t->t_expire > now) {
        systim_rtc1_rearm(t, now);
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
  LIST_INSERT_SORTED(&systim_rtc1_timers, t, t_link, systim_cmp);
  if(t == LIST_FIRST(&systim_rtc1_timers))
    systim_rtc1_rearm(t, clock_get_irq_blocked());
}


static void  __attribute__((constructor(131)))
nrf52_systim_init(void)
{
  irq_enable(17, IRQ_LEVEL_CLOCK);
  reg_wr(RTC1_BASE + RTC_INTENSET, 1 << 16); // Compare0 -> IRQ
}
