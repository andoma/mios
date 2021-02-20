#include <stdint.h>
#include <stdio.h>
#include <malloc.h>

#include "stm32h7_clk.h"

static volatile uint16_t *const FLASH_SIZE   = (volatile uint16_t *)0x1FF1E880;
static volatile uint32_t *const SYSCFG_PKGR  = (volatile uint32_t *)0x58000524;


static void  __attribute__((constructor(120)))
stm32h7_init(void)
{
  clk_enable(CLK_SYSCFG);

  uint32_t pkg = *SYSCFG_PKGR & 0xf;
  const char *pkgstr = "???";

  switch(pkg) {
  case 0: pkgstr = "LQFP100"; break;
  case 2: pkgstr = "TQFP144"; break;
  case 5: pkgstr = "TQFP176/UFBGA176"; break;
  case 8: pkgstr = "LQFP208/TFBGA240"; break;
  }

  const int flash_size = *FLASH_SIZE;
  printf("\nSTM32H7 (%s) (%d kB Flash)\n", pkgstr, flash_size);

  // DTCM
  extern unsigned long _ebss;
  void *DTCM_start = (void *)&_ebss;
  void *DTCM_end   = (void *)0x20000000 + 128 * 1024;
  heap_add_mem((long)DTCM_start, (long)DTCM_end, 0);


  long axi_sram_size = 0;

  // We infer RAM size based of flash memory sze
  switch(flash_size) {
  case 1024:
    axi_sram_size = 384;
    break;
  case 2048:
    axi_sram_size = 512;
    break;
  }

  if(axi_sram_size)
    heap_add_mem(0x24000000, 0x24000000 + axi_sram_size * 1024,
                 MEM_TYPE_DMA);
}
