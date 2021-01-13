#include <stdio.h>
#include <malloc.h>

#include "stm32f4_clk.h"

static volatile uint16_t *const FLASH_SIZE   = (volatile uint16_t *)0x1fff7a22;
static volatile uint32_t *const ACTLR        = (volatile uint32_t *)0xe000e008;
static volatile uint32_t *const DWT_CONTROL  = (volatile uint32_t *)0xE0001000;
static volatile uint32_t *const DWT_LAR      = (volatile uint32_t *)0xE0001FB0;
static volatile uint32_t *const SCB_DEMCR    = (volatile uint32_t *)0xE000EDFC;


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


  if(0) {
    // Enable this to disable instruction folding, write buffer and
    // interrupt of multi-cycle instructions
    // This can help generate more precise busfauls
    *ACTLR |= 7;
  }

  // Enable cycle counter
  *SCB_DEMCR |= 0x01000000;
  *DWT_LAR = 0xC5ACCE55; // unlock
  *DWT_CONTROL = 1;
}

