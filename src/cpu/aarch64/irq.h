#pragma once

#define IRQ_LEVEL_ALL      1
#define IRQ_LEVEL_HIGH     1

#define IRQ_LEVEL_SCHED    2

#define IRQ_LEVEL_CONSOLE  3

#define IRQ_LEVEL_IO       4

#define IRQ_LEVEL_NET      5

#define IRQ_LEVEL_CLOCK    13
#define IRQ_LEVEL_SWITCH   14

#define IRQ_PRI_LEVEL_SHIFT 4

#define IRQ_LEVEL_TO_PRI(x) ((x) << IRQ_PRI_LEVEL_SHIFT)

static inline unsigned int
irq_forbid(unsigned int level)
{
  unsigned int current;
  unsigned long daif;

  unsigned int pmr = IRQ_LEVEL_TO_PRI(level);

  asm volatile ("mrs %0, daif\n\t" : "=r" (daif));
  asm volatile ("msr daifset, #2\n\t");

  asm volatile ("mrs %0, icc_pmr_el1\n\r" : "=r"(current));
  if(pmr < current) {
    asm volatile ("msr icc_pmr_el1, %0\n\t" : : "r" (pmr));
  }
  asm volatile ("msr daif, %0\n\t" : : "r" (daif));
  return current;
}

static inline void
irq_permit(unsigned int old)
{
  asm volatile ("msr icc_pmr_el1, %0\n\t" : : "r" (old));
}

static inline unsigned int
irq_lower(void)
{
  unsigned int current;
  asm volatile ("mrs %0, icc_pmr_el1\n\r" : "=r"(current));
  asm volatile ("msr icc_pmr_el1, %0\n\t" : : "r" (0xff));
  return current;
}

static inline void
irq_off(void)
{
  asm volatile ("msr daifset, #2\n\t");
}

static inline void  __attribute__((always_inline))
schedule(void)
{
  long id;
  __asm__ volatile ("mrs %0, mpidr_el1" : "=r"(id));

  const long aff1 = (id >> 8) & 0xff;
  const long aff2 = (id >> 16) & 0xff;

  long x = 1;
  x |= (aff1 << 16);
  x |= (aff2 << 32);

  asm volatile ("msr icc_sgi1r_el1, %0\n\t" : : "r"(x));

  asm volatile("" ::: "memory");
}

static inline int can_sleep(void)
{
  unsigned long daif;
  asm volatile ("mrs %0, daif\n\t" : "=r" (daif));
  if(daif & 0x80)
    return 0;

  unsigned int spsel;
  asm volatile ("mrs %0, spsel\n\t" : "=r" (spsel));
  return spsel == 0; // spsel == 0 means we run on a thread
}


void irq_enable(int irq, int level);

void irq_enable_fn_arg(int irq, int level, void (*fn)(void *arg), void *arg);

void irq_enable_fn(int irq, int level, void (*fn)(void));

void irq_disable(int irq);

