#pragma once

#define IRQ_LEVEL_ALL      1
#define IRQ_LEVEL_HIGH     1

#define IRQ_LEVEL_SCHED    2

#define IRQ_LEVEL_CONSOLE  3

#define IRQ_LEVEL_IO       4

#define IRQ_LEVEL_NET      5

#define IRQ_LEVEL_CLOCK    6
#define IRQ_LEVEL_SWITCH   7

#define IRQ_PRI_LEVEL_SHIFT 5

#define IRQ_LEVEL_NONE -1 // Place holder value to signal no IRQ

#define IRQ_LEVEL_TO_PRI(x) ((x) << IRQ_PRI_LEVEL_SHIFT)

#include <mios/mios.h>

inline void __attribute__((always_inline))
irq_off(void)
{
  asm volatile ("cpsid i;isb;dsb");
}

#ifdef HAVE_BASEPRI

inline void  __attribute__((always_inline))
irq_ensure0(unsigned int level, const char *file, int line)
{
  unsigned int control;
  asm volatile ("mrs %0, control\n\t" : "=r" (control));

  if(!(control & 2))
    return;

  const unsigned int pri = IRQ_LEVEL_TO_PRI(level);
  unsigned int basepri;
  asm volatile ("mrs %0, basepri\n\t" : "=r" (basepri));


  if(!basepri || basepri > pri) {
    panic("Insuficient IRQ blocking at %s:%d level:%d basepri:0x%x control:0x%x\n",
          file, line, level, basepri, control);
  }
}


#define irq_ensure(l) irq_ensure0(l, __FILE__, __LINE__)


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

#else

inline void  __attribute__((always_inline))
irq_ensure0(unsigned int level, const char *file, int line)
{
  unsigned int control;
  asm volatile ("mrs %0, control\n\t" : "=r" (control));

  if(!(control & 2))
    return;

  unsigned int primask;
  asm volatile ("mrs %0, primask\n\t" : "=r" (primask));

  if(primask)
    return;

  panic("Insuficient IRQ blocking at %s:%d\n",
        file, line);
}


#define irq_ensure(l) irq_ensure0(l, __FILE__, __LINE__)

inline unsigned int  __attribute__((always_inline))
irq_forbid(int not_used)
{
  unsigned int old;
  asm volatile ("mrs %0, primask\n\t" : "=r" (old));
  asm volatile ("cpsid i");
  return old;
}

inline void  __attribute__((always_inline))
irq_permit(unsigned int old)
{
  asm volatile ("msr primask, %0\n\t" : : "r" (old));
}


inline unsigned int  __attribute__((always_inline))
irq_lower(void)
{
  unsigned int old;
  asm volatile ("mrs %0, primask\n\t" : "=r" (old));
  asm volatile ("cpsie i");
  return old;
}

#endif

static inline int  __attribute__((always_inline))
can_sleep(void)
{
  unsigned int primask;
  asm volatile ("mrs %0, primask\n\t" : "=r" (primask));

  unsigned int control;
  asm volatile ("mrs %0, control\n\t" : "=r" (control));
  return !!(control & 0x2) && primask == 0;
}

inline void  __attribute__((always_inline))
schedule(void)
{
  volatile unsigned int * const ICSR = (unsigned int *)0xe000ed04;
  *ICSR = 1 << 28; // Raise PendSV interrupt
  asm volatile("" ::: "memory");
}

void irq_enable(int irq, int level);

void irq_enable_fn(int irq, int level, void (*fn)(void));

void irq_enable_fn_arg(int irq, int level, void (*fn)(void *arg), void *arg);

void irq_enable_fn_fpu(int irq, int level, void (*fn)(void *arg), void *arg);

void irq_disable(int irq);

inline void  __attribute__((always_inline))
irq_ack(int irq)
{
  volatile unsigned int * const NVIC_ICPR = (unsigned int *)0xe000e280;
  NVIC_ICPR[(irq >> 5) & 7] |= 1 << (irq & 0x1f);
}

void systick_deinit(void);

void softreset(void) __attribute__((noreturn));
