#include "mios.h"
#include "irq.h"

void
irq(void)
{
  panic("Unexpected IRQ");
}

#include "irq_alias.h"


void
irq_init(void)
{



  asm volatile ("cpsie i\n\t"
                "isb\n\t");
}
