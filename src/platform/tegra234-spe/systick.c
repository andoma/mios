#include "cpu.h"
#include "reg.h"
#include "irq.h"

#include <mios/timer.h>
#include <mios/mios.h>

#include <stdio.h>


#define TKE_SHARED_BASE 0x0c0f0000
#define TKE_SHARED_TSC0 (TKE_SHARED_BASE + 0x0)
#define TKE_SHARED_TSC1 (TKE_SHARED_BASE + 0x4)
#define TKE_SHARED_USEC (TKE_SHARED_BASE + 0x8)

#define TIMER0_BASE 0x0c100000

#define TIMER_CR   0x0
#define TIMER_SR   0x4
#define TIMER_CSSR 0x8
#define TIMER_ATR  0xc

static struct timer_list timers;

static inline uint64_t
umul64x64hi(uint32_t b, uint32_t a, uint32_t d, uint32_t c)
{
  __asm__ ("umull   r4, r5, %[b], %[d]      \n\
            umull   %[d], r4, %[a], %[d]    \n\
            adds    r5, %[d]                \n\
            umull   %[d], %[a], %[a], %[c]  \n\
            adcs    r4, %[d]                \n\
            adc     %[a], #0                \n\
            umull   %[c], %[b], %[b], %[c]  \n\
            adds    r5, %[c]                \n\
            adcs    %[b], r4                \n\
            adc     %[a], #0                \n"
           : [a] "+r" (a), [b] "+r" (b), [c] "+r" (c), [d] "+r" (d)
           : : "r4", "r5");
    return (uint64_t) a << 32 | b;
}


uint64_t
clock_get_irq_blocked(void)
{
  while(1) {
    uint32_t hi = reg_rd(TKE_SHARED_TSC1);
    uint32_t lo = reg_rd(TKE_SHARED_TSC0);
    if(reg_rd(TKE_SHARED_TSC1) == hi) {
      // Scale by (100 / 3125) using multiplication of its reciprocal
      return umul64x64hi(lo, hi, 0x78d4fdc0, 0x83126e9);
    }
  }
}

uint64_t
clock_get(void)
{
  return clock_get_irq_blocked();
}

void
udelay(unsigned int usec)
{
  uint64_t deadline = clock_get() + usec;
  while(clock_get() < deadline) {}
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



static void
timer0_irq(void)
{
  reg_wr(TIMER0_BASE + TIMER_SR, (1 << 30));
  timer_dispatch(&timers, clock_get_irq_blocked());
}


static void  __attribute__((constructor(122)))
systick_init(void)
{
  irq_enable_fn(2, IRQ_LEVEL_CLOCK, timer0_irq);

  reg_wr(TIMER0_BASE + TIMER_CSSR, 0);
  reg_wr(TIMER0_BASE + TIMER_CR,
         (1 << 31) |
         (1 << 30) |
         10000);
}
