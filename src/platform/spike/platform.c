#include <stdint.h>

#include "platform.h"

volatile long tohost;
volatile long fromhost;


void *
platform_heap_end(void)
{
  return (void *)0x80100000;
}

void
platform_init(void)
{
}
