#include <stdio.h>
#include <malloc.h>
#include <stdint.h>

static volatile uint16_t *const FLASH_SIZE   = (volatile uint16_t *)0x1fff75e0;

static void  __attribute__((constructor(120)))
stm32wb_init(void)
{
  extern unsigned long _ebss;

  void *SRAM2_start = (void *)&_ebss;
  void *SRAM2_end   = (void *)0x20000000 + 64 * 1024;

  heap_add_mem((long)SRAM2_start, (long)SRAM2_end, MEM_TYPE_DMA);

  printf("\nSTM32WB (%d kB Flash)\n", *FLASH_SIZE);
}
