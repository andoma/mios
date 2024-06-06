#include "cpu.h"
#include "reg.h"
#include "irq.h"

#include <mios/timer.h>
#include <mios/mios.h>

#include <stdio.h>

#define PTLOAD   0x600
#define PTCNTR   0x604
#define PTCTRL   0x608
#define PTSTATUS 0x60c

#define HZ 100
#define TICKS_PER_US ((CPU_TIMER_CLOCK + 999999) / 1000000)
#define TICKS_PER_HZ ((CPU_TIMER_CLOCK + HZ - 1) / HZ)


static struct timer_list timers;

uint64_t clock;

static void
tick_irq(void)
{
  uint32_t pbase = cpu_get_periphbase();
  int ev = reg_rd(pbase + PTSTATUS);
  if(ev & 1) {
    clock += 1000000 / HZ;
    reg_wr(pbase + PTSTATUS, 1);
  }

  const uint64_t now = clock;
  timer_dispatch(&timers, now);
}

uint64_t
clock_get_irq_blocked(void)
{
  uint32_t pbase = cpu_get_periphbase();

  while(1) {
    uint32_t v = reg_rd(pbase + PTCNTR);
    uint32_t remain = v / TICKS_PER_US;
    uint64_t c = clock;

    c += (1000000 / HZ) - remain;

    if(unlikely(reg_rd(pbase + PTSTATUS))) {
      clock += 1000000 / HZ;
      reg_wr(pbase + PTSTATUS, 1);
      continue;
    }
    return c;
  }

  return 0;
}


uint64_t
clock_get(void)
{
  int s = irq_forbid(IRQ_LEVEL_CLOCK);
  uint64_t r = clock_get_irq_blocked();
  irq_permit(s);
  return r;
}


static int
timer_cmp(const timer_t *a, const timer_t *b)
{
  return a->t_expire > b->t_expire;
}


// IRQ_LEVEL_CLOCK must be blocked
__attribute__((weak))
void
timer_arm_abs(timer_t *t, uint64_t expire)
{
  if(t->t_expire)
    LIST_REMOVE(t, t_link);

  t->t_expire = expire;

  LIST_INSERT_SORTED(&timers, t, t_link, timer_cmp);
}



static void  __attribute__((constructor(122)))
systick_init(void)
{
  uint32_t pbase = cpu_get_periphbase();
  reg_wr(pbase + PTLOAD, TICKS_PER_HZ);
  reg_wr(pbase + PTCTRL, 0b111);
  irq_enable_fn(29, IRQ_LEVEL_CLOCK, tick_irq);
}
