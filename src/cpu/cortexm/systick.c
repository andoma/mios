#include <stdint.h>
#include <stdio.h>
#include <unistd.h>

#include <mios/mios.h>
#include <mios/timer.h>

#include "irq.h"
#include "systick.h"
#include "cpu.h"

#ifdef CPU_SYSTICK_RVR

#define TICKS_PER_US ((CPU_SYSTICK_RVR + 999999) / 1000000)
#define TICKS_PER_HZ ((CPU_SYSTICK_RVR + HZ - 1) / HZ)

_Static_assert(TICKS_PER_HZ < 0xffffff);

#endif

static volatile unsigned int * const SYST_CSR = (unsigned int *)0xe000e010;
static volatile unsigned int * const SYST_RVR = (unsigned int *)0xe000e014;
static volatile unsigned int * const SYST_VAL = (unsigned int *)0xe000e018;

static struct timer_list timers;

uint64_t clock;

void
exc_systick(void)
{
  if(likely(*SYST_CSR & 0x10000)) {
    clock += 1000000 / HZ;
  }

  const uint64_t now = clock;
  timer_dispatch(&timers, now);
}


#ifdef CPU_SYSTICK_RVR

uint64_t
clock_get_irq_blocked(void)
{
  while(1) {
    uint32_t v = *SYST_VAL;
    uint32_t remain = v / TICKS_PER_US;
    uint64_t c = clock;

    c += (1000000 / HZ) - remain;

    if(unlikely(*SYST_CSR & 0x10000)) {
      clock += 1000000 / HZ;
      continue;
    }
    return c;
  }
}

#endif

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


uint64_t
clock_get(void)
{
  int s = irq_forbid(IRQ_LEVEL_CLOCK);
  uint64_t r = clock_get_irq_blocked();
  irq_permit(s);
  return r;
}


void
udelay(unsigned int usec)
{
  int s = irq_forbid(IRQ_LEVEL_CLOCK);
  uint64_t deadline = clock_get_irq_blocked() + usec;
  while(clock_get_irq_blocked() < deadline) {}
  irq_permit(s);
}

#ifdef CPU_SYSTICK_RVR

static void __attribute__((constructor(130)))
systick_init(void)
{
  *SYST_RVR = TICKS_PER_HZ;
  *SYST_VAL = 0;
  *SYST_CSR = 7;
}

#endif

static void  __attribute__((destructor(130)))
systick_deinit(void)
{
  *SYST_RVR = 0;
  *SYST_VAL = 0;
  *SYST_CSR = 0;
}

