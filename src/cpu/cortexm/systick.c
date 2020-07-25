#include <stdint.h>
#include <stdio.h>
#include "timer.h"

#include "clk_config.h"
#include "sys.h"
#include "irq.h"

#define HZ 100
#define TICKS_PER_US (SYSTICK_RVR / 1000000)
#define TICKS_PER_HZ (SYSTICK_RVR / HZ)

static volatile unsigned int * const SYST_CSR =   (unsigned int *)0xe000e010;
static volatile unsigned int * const SYST_RVR =   (unsigned int *)0xe000e014;
static volatile unsigned int * const SYST_VAL =   (unsigned int *)0xe000e018;
//static volatile unsigned int * const SYST_CALIB = (unsigned int *)0xe000e01c;

LIST_HEAD(timer_list, timer);

static struct timer_list timers;

static uint64_t clock;

void
exc_systick(void)
{
  if(*SYST_CSR & 0x10000) {
    clock += 1000000 / HZ;
  }

  const uint64_t now = clock;
  struct timer_list pending;
  LIST_INIT(&pending);
  timer_t *t, *n;
  for(t = LIST_FIRST(&timers); t != NULL; t = n) {
    n = LIST_NEXT(t, t_link);
    if(t->t_expire < now) {
      LIST_REMOVE(t, t_link);
      LIST_INSERT_HEAD(&pending, t, t_link);
    }
  }

  while((t = LIST_FIRST(&pending)) != NULL) {
    LIST_REMOVE(t, t_link);
    t->t_expire = 0;
    t->t_cb(t->t_opaque);
  }
}


static uint64_t
clock_get_irq_blocked(void)
{
  while(1) {
    uint32_t v = *SYST_VAL;
    uint32_t remain = (1000000 / HZ) * v / TICKS_PER_HZ;
    uint64_t c = clock;

    c += (1000000 / HZ) - remain;

    if(*SYST_CSR & 0x10000) {
      clock += 1000000 / HZ;
      continue;
    }
    return c;
  }
}



// IRQ_LEVEL_CLOCK must be blocked
void
timer_arm_abs(timer_t *t, uint64_t expire)
{
  if(t->t_expire)
    LIST_REMOVE(t, t_link);

  t->t_expire = expire;
  LIST_INSERT_HEAD(&timers, t, t_link);
}

void
timer_arm(timer_t *t, unsigned int delta)
{
  timer_arm_abs(t, clock_get_irq_blocked() + delta);
}


// IRQ_LEVEL_CLOCK must be blocked
int
timer_disarm(timer_t *t)
{
  if(!t->t_expire)
    return 1;
  LIST_REMOVE(t, t_link);
  t->t_expire = 0;
  return 0;
}



//static volatile unsigned int * const SYST_SHPR3 = (unsigned int *)0xe000ed20;

static void __attribute__((constructor(130)))
timer_init(void)
{
  *SYST_RVR = TICKS_PER_HZ - 1;
  *SYST_VAL = 0;
  *SYST_CSR = 7;
}



uint64_t
clock_get(void)
{
  int s = irq_forbid(IRQ_LEVEL_CLOCK);
  uint64_t r = clock_get_irq_blocked();
  irq_permit(s);
  return r;
}
