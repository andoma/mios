#pragma once

#define IRQ_LEVEL_SCHED 0
#define IRQ_LEVEL_CLOCK 1

#define IRQ_LEVEL_PRI_SHIFT 5


#define IRQ_PRI(x) ((x) << IRQ_LEVEL_PRI_SHIFT)

inline uint32_t
irq_disable(uint32_t level)
{
  uint32_t old;
  asm volatile ("mrs %0, basepri\n\t" : "=r" (old));
  asm volatile ("msr basepri_max, %0\n\t" : : "r" (IRQ_PRI(level)));
  return old;
}

inline void
irq_enable(uint32_t pri)
{
  asm volatile ("msr basepri, %0\n\t" : : "r" (pri));
}


inline void
irq_setpri(uint32_t pri)
{
  asm volatile ("msr basepri, %0\n\t" : : "r" (pri));
}


inline uint32_t
irq_getpri(void)
{
  uint32_t pri;
  asm volatile ("mrs %0, basepri\n\t" : "=r" (pri));
  return pri;
}

