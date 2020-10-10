#include <stdio.h>
#include <malloc.h>

#include "stm32f4_clk.h"

static volatile unsigned short * const FLASH_SIZE = (unsigned short *)0x1fff7a22;


static void  __attribute__((constructor(120)))
stm32f4_init(void)
{
  extern unsigned long _ebss;

  void *SRAM1_start = (void *)&_ebss;
  void *SRAM1_end   = (void *)0x20000000 + 112 * 1024;

  printf("\nSTM32F4 (%d kB Flash)\n", *FLASH_SIZE);

  // SRAM1
  heap_add_mem((long)SRAM1_start, (long)SRAM1_end, MEM_TYPE_DMA);

   // CCM
  clk_enable(CLK_CCMDATARAMEN);
  heap_add_mem(0x10000000, 0x10010000, MEM_TYPE_LOCAL);
}

