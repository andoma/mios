#include <stdint.h>

#include "mios.h"
#include "irq.h"

void
irq(void)
{
  panic("Unexpected IRQ");
}

#include "irq_alias.h"


static volatile unsigned int * const NVIC_ISER = (unsigned int *)0xe000e100;
static volatile unsigned int * const NVIC_ICER = (unsigned int *)0xe000e180;
static volatile uint8_t * const NVIC_IPR  = (uint8_t *)0xe000e400;

void
irq_enable(int irq, int level)
{
  NVIC_IPR[irq] = IRQ_LEVEL_TO_PRI(level);
  NVIC_ISER[(irq >> 5) & 7] |= 1 << (irq & 0x1f);
}

void
irq_disable(int irq)
{
  NVIC_ICER[(irq >> 5) & 7] |= 1 << (irq & 0x1f);
}


static volatile unsigned int * const SYST_SHPR3 = (unsigned int *)0xe000ed20;
static volatile unsigned int * const VTOR  = (unsigned int *)0xe000ed08;


static void __attribute__((constructor(140)))
irq_init(void)
{
  extern int *vectors;
  *VTOR = (uint32_t)&vectors;

  *SYST_SHPR3 =
    (IRQ_LEVEL_TO_PRI(IRQ_LEVEL_CLOCK) << 24) |
    (IRQ_LEVEL_TO_PRI(IRQ_LEVEL_SWITCH) << 16);
}
