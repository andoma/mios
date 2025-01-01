#include <stdio.h>
#include <malloc.h>

#include "drivers/pl011.h"

static void __attribute__((constructor(101)))
board_init_early(void)
{
  heap_add_mem(0x41000000, 0x42000000, MEM_TYPE_DMA);
  stdio = pl011_uart_init(0x09000000, 115200, 33);
}
