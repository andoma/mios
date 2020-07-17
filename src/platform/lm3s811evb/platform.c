#include <stdint.h>

#include "platform.h"

void *
platform_heap_end(void)
{
  return (void *)0x20008000;
}


void lm3s811evb_console_init(void);

void
platform_init(void)
{
  lm3s811evb_console_init();
}
