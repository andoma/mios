#include <stdio.h>

#include "platform.h"

#include "irq.h"

static void
uart_putc(void *p, char c)
{
  int s = irq_forbid(IRQ_LEVEL_SCHED);

  tohost = 1ULL << 56 | 1ULL << 48 | c;
  while(tohost) {}

  irq_permit(s);
}

void
platform_console_init_early(void)
{
  init_printf(NULL, uart_putc);
}
