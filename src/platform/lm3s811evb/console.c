#include <stdio.h>

#include "platform.h"


static void
uart_putc(void *p, char c)
{
  *(int *)p = c;
}

void
platform_console_init(void)
{
  init_printf((unsigned int *)0x4000c000, uart_putc);
}
