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


// VIC0 interrupts
#define IRQ_WDTFIQ   0
#define IRQ_WDTIRQ   1
#define IRQ_TIMER0   2
#define IRQ_TIMER1   3
#define IRQ_TIMER2   4
#define IRQ_TIMER3   5
#define IRQ_MBOX     6
#define IRQ_GTE      7
#define IRQ_PMU      8
#define IRQ_DMA0     9
#define IRQ_DMA1     10
#define IRQ_DMA2     11
#define IRQ_DMA3     12
#define IRQ_DMA4     13
#define IRQ_DMA5     14
#define IRQ_DMA6     15
#define IRQ_DMA7     16
#define IRQ_V0RSVD17 17
#define IRQ_I2C2     18
#define IRQ_I2C3     19
#define IRQ_SPI      20
#define IRQ_DMIC     21
#define IRQ_UART_1   22
#define IRQ_UART_J   23
#define IRQ_CAN1_0   24
#define IRQ_CAN1_1   25
#define IRQ_CAN2_0   26
#define IRQ_CAN2_1   27
#define IRQ_LIC0     28
#define IRQ_LIC1     29
#define IRQ_LIC2     30
#define IRQ_LIC3     31

// VIC1 interrupts
#define IRQ_NOC_ERR   32
#define IRQ_GPIO      33
#define IRQ_WAKE0     34
#define IRQ_PMC       35
#define IRQ_V1RSVD4   36
#define IRQ_PM        37
#define IRQ_FPUINT    38
#define IRQ_V1RSVD7   39
#define IRQ_ACTMON    40
#define IRQ_AOWDT     41
#define IRQ_TOP0_HSP_DB 42
#define IRQ_CTIIRQ   43
#define IRQ_NOC_SEC  44
#define IRQ_CAR      45
#define IRQ_UART6    46
#define IRQ_UART8    47
#define IRQ_GPIO_3   48
#define IRQ_CEC      49
#define IRQ_V1RSVD18 50
#define IRQ_V1RSVD19 51
#define IRQ_V1RSVD20 52
#define IRQ_V1RSVD21 53
#define IRQ_V1RSVD22 54
#define IRQ_V1RSVD23 55
#define IRQ_V1RSVD24 56
#define IRQ_V1RSVD25 57
#define IRQ_V1RSVD26 58
#define IRQ_V1RSVD27 59
#define IRQ_V1RSVD28 60
#define IRQ_V1RSVD29 61
#define IRQ_V1RSVD30 62
#define IRQ_V1RSVD31 63


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
