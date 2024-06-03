#pragma once

#include "cpu.h"
#include "reg.h"

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

void irq_enable(int irq, int level);

void irq_enable_fn_arg(int irq, int level, void (*fn)(void *arg), void *arg);

void irq_enable_fn(int irq, int level, void (*fn)(void));

void irq_disable(int irq);


#define ICPICR     0x100
#define ICCIPMR    0x104
#define ICCIAR     0x10c
#define ICCEOIR    0x110
#define ICCRPR     0x114
#define ICPIIR     0x1fc

#define GICD_CTRL  0x1000
#define GICD_TYPER 0x1004
#define GICD_IIDR  0x1008
#define GICD_ISENABLER(x)  (0x1100 + (x) * 4)
#define GICD_ICENABLER(x)  (0x1180 + (x) * 4)
#define GICD_ISPENDR(x)    (0x1200 + (x) * 4)
#define GICD_ICPENDR(x)    (0x1280 + (x) * 4)
#define GICD_IPRIORITYR(x) (0x1400 + (x))
#define GICD_ITARGETSR(x)  (0x1800 + (x))
#define GICD_ICFGR(x)      (0x1c00 + (x) * 4)
#define GICD_SGIR  0x1f00

__attribute__((always_inline))
static inline unsigned int
irq_forbid(unsigned int level)
{
  uint32_t cpsr;
  uint32_t pri = IRQ_LEVEL_TO_PRI(level);
  uint32_t pbase = cpu_get_periphbase();

  asm volatile ("mrs %0, cpsr\n\t" : "=r" (cpsr));
  asm volatile ("msr cpsr, %0\n\r" :: "r" (cpsr | 0x80));

  uint32_t pmr = reg_rd(pbase + ICCIPMR);
  if(pri < pmr) {
    reg_wr(pbase + ICCIPMR, pri);
    asm volatile("isb" ::: "memory");
  }
  asm volatile ("msr cpsr, %0\n\r" :: "r" (cpsr));
  return pmr;
}

__attribute__((always_inline))
static inline void
irq_permit(unsigned int old)
{
  uint32_t pbase = cpu_get_periphbase();
  reg_wr(pbase + ICCIPMR, old);
}

__attribute__((always_inline))
static inline unsigned int
irq_lower(void)
{
  uint32_t pbase = cpu_get_periphbase();
  uint32_t old = reg_rd(pbase + ICCIPMR);
  reg_wr(pbase + ICCIPMR, 0xf8);
  asm volatile("isb" ::: "memory");
  return old;
}

__attribute__((always_inline))
static inline void
schedule(void)
{
  uint32_t pbase = cpu_get_periphbase();
  reg_wr(pbase + GICD_SGIR, (0b10 << 24));
  asm volatile("" ::: "memory");
}

__attribute__((always_inline))
static inline int
can_sleep(void)
{
  uint32_t pbase = cpu_get_periphbase();
  return reg_rd(pbase + ICCRPR) >= 0xf8;
}
