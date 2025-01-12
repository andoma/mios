#include <unistd.h>
#include <mios/timer.h>

#include <mios/mios.h>
#include <stdio.h>

#include "irq.h"

static uint64_t nxt_timer;
static uint64_t hz;

static struct timer_list timers;


uint64_t
clock_get_irq_blocked(void)
{
  uint32_t freq;
  asm volatile ("mrs %0, cntfrq_el0\n\r" : "=r"(freq));

  uint64_t cntr;
  asm volatile ("mrs %0, cntvct_el0\n\r" : "=r"(cntr));

  return 1000000ull * cntr / freq;
}

uint64_t clock_get(void)
{
  return clock_get_irq_blocked();
}

static int
timer_cmp(const timer_t *a, const timer_t *b)
{
  return a->t_expire > b->t_expire;
}

void
timer_arm_abs(timer_t *t, uint64_t expire)
{
  if(t->t_expire)
    LIST_REMOVE(t, t_link);

  t->t_expire = expire;

  LIST_INSERT_SORTED(&timers, t, t_link, timer_cmp);
}

static void
timer_virt(void *arg)
{
  nxt_timer += hz;
  asm volatile ("msr cntv_cval_el0, %0\n\t" : : "r"(nxt_timer));
  uint64_t now = clock_get_irq_blocked();
  timer_dispatch(&timers, now);
}


static void  __attribute__((constructor(103)))
clock_init(void)
{
  uint32_t freq;
  asm volatile ("mrs %0, cntfrq_el0\n\r" : "=r"(freq));
  printf("Timer frequency: %d\n", freq);
  nxt_timer = clock_get_irq_blocked();
  printf("System clock is %ld\n", nxt_timer);
  hz = freq / 10;

  irq_enable_fn_arg(27, IRQ_LEVEL_CLOCK, timer_virt, NULL);

  uint64_t cntr;
  asm volatile ("mrs %0, cntvct_el0\n\r" : "=r"(cntr));
  cntr += hz;
  asm volatile ("msr cntv_cval_el0, %0\n\t" : : "r"(nxt_timer));

  // Enable timer
  asm volatile("msr cntv_ctl_el0, %0\n\t" : : "r" (1));
}

void
udelay(unsigned int usec)
{
  int s = irq_forbid(IRQ_LEVEL_CLOCK);
  uint64_t deadline = clock_get_irq_blocked() + usec;
  while(clock_get_irq_blocked() < deadline) {}
  irq_permit(s);
}
