#pragma once

#define IRQ_LEVEL_SCHED 0
#define IRQ_LEVEL_CLOCK 1

#define IRQ_LEVEL_PRI_SHIFT 5


#define IRQ_PRI(x) ((x) << IRQ_LEVEL_PRI_SHIFT)

inline unsigned int
irq_disable(unsigned int level)
{
  unsigned int old;
  asm volatile ("mrs %0, basepri\n\t" : "=r" (old));
  asm volatile ("msr basepri_max, %0\n\t" : : "r" (IRQ_PRI(level)));
  return old;
}

inline void
irq_enable(unsigned int pri)
{
  asm volatile ("msr basepri, %0\n\t" : : "r" (pri));
}


inline void
irq_setpri(unsigned int pri)
{
  asm volatile ("msr basepri, %0\n\t" : : "r" (pri));
}


inline unsigned int irq_getpri(void)
{
  unsigned int pri;
  asm volatile ("mrs %0, basepri\n\t" : "=r" (pri));
  return pri;
}

void irq_init(void);
