#include <stdio.h>
#include <malloc.h>
#include <stdint.h>

#include "cpu.h"

#include "pl011.h"

static void  __attribute__((constructor(110)))
vexpress_a9_init_heap(void)
{
  uint32_t ramsize = 32 * 1024 * 1024;
  void *RAM_end = (void *)0x60000000 + ramsize;
  heap_add_mem(HEAP_START_EBSS, (long)RAM_end, MEM_TYPE_DMA);


  stdio = pl011_uart_init(0x10009000, 115200, 37);
}
