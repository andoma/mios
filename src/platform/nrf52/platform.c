#include <stdint.h>

#include "platform.h"

void *
platform_heap_end(void)
{
  volatile unsigned int * const RAMSIZE = (unsigned int *)0x1000010c;
  uint32_t r = *RAMSIZE;
  return (void *)0x20000000 + r * 1024;
}
