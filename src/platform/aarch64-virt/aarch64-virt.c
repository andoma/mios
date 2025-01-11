#include <stdio.h>
#include <malloc.h>

#include "drivers/pl011.h"

static void __attribute__((constructor(101)))
board_init_early(void)
{
  heap_add_mem(HEAP_START_EBSS, 0xffff000000200000ull + 2 * 1024 * 1024,
               MEM_TYPE_DMA);
  stdio = pl011_uart_init(0x09000000, 115200, 33);

  long wut;
  asm volatile("mrs %0, ID_AA64MMFR0_EL1\n\r" : "=r"(wut));
  printf("ID_AA64MMFR0_EL1 = %lx\n", wut);
}
