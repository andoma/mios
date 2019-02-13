#include <stdint.h>

#include "platform.h"

#include "nrf52.h"

void *
platform_heap_end(void)
{
  volatile unsigned int * const RAMSIZE = (unsigned int *)0x1000010c;
  uint32_t r = *RAMSIZE;
  return (void *)0x20000000 + r * 1024;
}


void
platform_init(void)
{
  nrf52_console_init();
}
