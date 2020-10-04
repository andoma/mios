#pragma once

#define IRQ_LEVEL_SCHED    2

#define IRQ_LEVEL_CONSOLE  3

#define IRQ_LEVEL_IO       4

#define IRQ_LEVEL_DMA      5

#define IRQ_LEVEL_CLOCK    6
#define IRQ_LEVEL_SWITCH   7

#define IRQ_PRI_LEVEL_SHIFT 5


#define IRQ_LEVEL_TO_PRI(x) ((x) << IRQ_PRI_LEVEL_SHIFT)

inline void  __attribute__((always_inline))
irq_off(void)
{
  asm volatile ("cpsid i\n\t");
}

inline unsigned int  __attribute__((always_inline))
irq_forbid(unsigned int level)
{
  unsigned int old;
  asm volatile ("mrs %0, basepri\n\t" : "=r" (old));
  asm volatile ("msr basepri_max, %0\n\t" : : "r" (IRQ_LEVEL_TO_PRI(level)));
  return old;
}

inline void  __attribute__((always_inline))
irq_permit(unsigned int pri)
{
  asm volatile ("msr basepri, %0\n\t" : : "r" (pri));
}


inline unsigned int  __attribute__((always_inline))
irq_lower(void)
{
  unsigned int old;
  asm volatile ("mrs %0, basepri\n\t" : "=r" (old));
  asm volatile ("msr basepri, %0\n\t" : : "r" (0));
  return old;
}


void irq_enable(int irq, int level);

void irq_disable(int irq);

inline void  __attribute__((always_inline))
irq_ack(int irq)
{
  volatile unsigned int * const NVIC_ICPR = (unsigned int *)0xe000e280;
  NVIC_ICPR[(irq >> 5) & 7] |= 1 << (irq & 0x1f);
}


static inline int  __attribute__((always_inline))
can_sleep(void)
{
  unsigned int basepri;
  asm volatile ("mrs %0, basepri\n\t" : "=r" (basepri));

  unsigned int control;
  asm volatile ("mrs %0, control\n\t" : "=r" (control));
  return !!(control & 0x2) && basepri == 0;
}

