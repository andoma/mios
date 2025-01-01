#pragma once

#define IRQ_LEVEL_ALL      1
#define IRQ_LEVEL_HIGH     1

#define IRQ_LEVEL_SCHED    2

#define IRQ_LEVEL_CONSOLE  3

#define IRQ_LEVEL_IO       4

#define IRQ_LEVEL_NET      5

#define IRQ_LEVEL_CLOCK    6
#define IRQ_LEVEL_SWITCH   7

static inline unsigned int
irq_forbid(unsigned int level)
{
  long current;
  asm volatile ("mrs %0, daif\n\t" : "=r" (current));
  asm volatile ("msr daifset, #7\n\t");
  return current;
}

static inline void
irq_permit(unsigned int old)
{
  asm volatile ("msr daif, %0\n\t" : : "r" (old));
}

static inline unsigned int
irq_lower(void)
{
  long current;
  asm volatile ("mrs %0, daif\n\t" : "=r" (current));
  asm volatile ("msr daifclr, #7\n\t");
  return current;
}

static inline void
irq_off(void)
{
  asm volatile ("msr daifset, #7\n\t");
}

inline void  __attribute__((always_inline))
schedule(void)
{
}
