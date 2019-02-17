#pragma once

#define IRQ_LEVEL_SCHED    2

#define IRQ_LEVEL_CONSOLE  3

#define IRQ_LEVEL_CLOCK    6
#define IRQ_LEVEL_SWITCH   7


static inline unsigned int
irq_forbid(unsigned int level)
{
  long new = 0x888;
  long old;
  asm volatile ("csrrc %0, mie, %1\n\t" : "=r" (old) : "r"(new));
  return old & 0x888;
}

static inline void
irq_permit(unsigned int mask)
{
  asm volatile ("csrs mie, %0\n\t" : : "r"(mask));
}

static inline void
irq_off(void)
{
  __asm volatile("csrc mstatus,%0"::"r"(0x8)); // Disable all interrupts
}

static inline void
irq_init(void)
{
}

static inline int
irq_lower(void)
{
  long new = 0x888;
  long old;
  asm volatile ("csrrs %0, mie, %1\n\t" : "=r" (old) : "r"(new));
  return old & 0x888;
}
