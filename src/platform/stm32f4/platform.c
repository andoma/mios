#include <stdint.h>
#include <stddef.h>
#include <stdio.h>

#include "irq.h"
#include "platform/platform.h"
#include "stm32f4.h"

#include "gpio.h"
#include "task.h"
#include "mios.h"


void *
platform_heap_end(void)
{
  return (void *)0x20000000 + 112 * 1024;
}

