#pragma once

#include "cpu.h"
#include "reg.h"

#define IRQ_LEVEL_ALL      1
#define IRQ_LEVEL_HIGH     1

#define IRQ_LEVEL_SCHED    2

#define IRQ_LEVEL_CONSOLE  3

#define IRQ_LEVEL_IO       4

#define IRQ_LEVEL_NET      5

#define IRQ_LEVEL_CLOCK    14
#define IRQ_LEVEL_SWITCH   15

#define LIC_IRQ(x) (0x10000 | (x))

#define VIC_BASE(x) (0xc020000 + 0x10000 * (x))

#define VIC_INT_ENABLE         0x010
#define VIC_INT_DISABLE        0x014
#define VIC_SW_SET             0x018
#define VIC_SW_PRIORITY_MASK   0x024
#define VIC_VECTOR_ADDRESS(x) (0x100 + (x) * 4)
#define VIC_PRIORITY(x)       (0x200 + (x) * 4)

void irq_enable(int irq, int level);

void irq_enable_fn_arg(int irq, int level, void (*fn)(void *arg), void *arg);

void irq_enable_fn(int irq, int level, void (*fn)(void));

void irq_disable(int irq);

extern uint32_t irq_current_mask;

__attribute__((always_inline))
static inline unsigned int
irq_forbid(unsigned int level)
{
  uint32_t new_mask = ~(int16_t)0x8000 >> (15 - level);

  uint32_t old = __atomic_fetch_and(&irq_current_mask, new_mask,
                                    __ATOMIC_SEQ_CST);

  reg_wr(VIC_BASE(0) + VIC_SW_PRIORITY_MASK, old & new_mask);
  reg_wr(VIC_BASE(1) + VIC_SW_PRIORITY_MASK, old & new_mask);
  asm volatile ("dmb");
  return old;
}

__attribute__((always_inline))
static inline void
irq_permit(unsigned int old)
{
  __atomic_store_n(&irq_current_mask, old, __ATOMIC_SEQ_CST);
  reg_wr(VIC_BASE(0) + VIC_SW_PRIORITY_MASK, old);
  reg_wr(VIC_BASE(1) + VIC_SW_PRIORITY_MASK, old);
  asm volatile ("dmb");
}

__attribute__((always_inline))
static inline unsigned int
irq_lower(void)
{
  uint32_t old = __atomic_fetch_or(&irq_current_mask, 0xffff,
                                    __ATOMIC_SEQ_CST);
  reg_wr(VIC_BASE(0) + VIC_SW_PRIORITY_MASK, 0xffff);
  reg_wr(VIC_BASE(1) + VIC_SW_PRIORITY_MASK, 0xffff);
  asm volatile ("dmb");
  return old;
}

__attribute__((always_inline))
static inline void
schedule(void)
{
  reg_wr(VIC_BASE(0) + VIC_SW_SET, 1 << 17);
}

__attribute__((always_inline))
static inline int
can_sleep(void)
{
  const uint32_t cpsr = cpu_get_cpsr();

  // We may sleep if MODE=SYS (regular thread exection) and IRQs are not
  // disabled (bit 7 (0x80))
  return (cpsr & 0x9f) == 0x1f;
}

__attribute__((always_inline))
static inline void
irq_off(void)
{
  asm volatile ("cpsid i;isb;dsb");
}
