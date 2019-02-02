#pragma once

#define IRQ_LEVEL_SVC      4
#define IRQ_LEVEL_CONSOLE  4
#define IRQ_LEVEL_CLOCK    4
#define IRQ_LEVEL_SWITCH   4

#define IRQ_PRI_LEVEL_SHIFT 5


#define IRQ_LEVEL_TO_PRI(x) ((x) << IRQ_PRI_LEVEL_SHIFT)

inline unsigned int
irq_forbid(unsigned int level)
{
  unsigned int old;
  asm volatile ("mrs %0, basepri\n\t" : "=r" (old));
  asm volatile ("msr basepri_max, %0\n\t" : : "r" (IRQ_LEVEL_TO_PRI(level)));
  return old;
}

inline void
irq_permit(unsigned int pri)
{
  asm volatile ("msr basepri, %0\n\t" : : "r" (pri));
}


inline void
irq_forbid_restore(unsigned int pri)
{
  asm volatile ("msr basepri, %0\n\t" : : "r" (pri));
}

inline unsigned int
irq_forbid_save(void)
{
  unsigned int pri;
  asm volatile ("mrs %0, basepri\n\t" : "=r" (pri));
  return pri;
}

void irq_enable(int irq, int level);

void irq_disable(int irq);

void irq_raise(int irq);

inline void
irq_ack(int irq)
{
  volatile unsigned int * const NVIC_ICPR = (unsigned int *)0xe000e280;
  NVIC_ICPR[(irq >> 5) & 7] |= 1 << (irq & 0x1f);
}





void irq_init(void);

