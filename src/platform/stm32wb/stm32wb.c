#include <stdio.h>
#include <malloc.h>
#include <stdint.h>

static volatile uint16_t *const FLASH_SIZE   = (volatile uint16_t *)0x1fff75e0;

static void  __attribute__((constructor(120)))
stm32wb_init(void)
{
  void *SRAM2_end   = (void *)0x20000000 + 64 * 1024;

  heap_add_mem(HEAP_START_EBSS, (long)SRAM2_end,
               MEM_TYPE_DMA | MEM_TYPE_VECTOR_TABLE, 0);

  printf("\nSTM32WB (%d kB Flash)\n", *FLASH_SIZE);
}
