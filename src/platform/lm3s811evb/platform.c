#include <stdint.h>

#include "platform/platform.h"

void *
platform_heap_end(void)
{
  return (void *)0x20008000;
}
