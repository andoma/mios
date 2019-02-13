#include <stdint.h>

#include "platform.h"

void *
platform_heap_end(void)
{
  return (void *)0x20008000;
}

void
platform_init(void)
{
}
