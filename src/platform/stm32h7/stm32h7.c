#include <stdint.h>
#include <stdio.h>
#include <malloc.h>

#include <net/pbuf.h>

#include "stm32h7_clk.h"

static volatile uint16_t *const FLASH_SIZE   = (volatile uint16_t *)0x1FF1E880;
static volatile uint32_t *const LINE_ID      = (volatile uint32_t *)0x1FF1E8c0;
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
  const uint32_t line_id = *LINE_ID;

  printf("\nSTM32%c%c%c%c (%s) (%d kB Flash)\n",
         line_id >> 24,
         line_id >> 16,
         line_id >> 8,
         line_id,
         pkgstr, flash_size);


  long axi_sram_size = 0;

  switch(line_id) {
  case 0x48373233: // STM32H723
    axi_sram_size = 128;
    break;

  default:
    // Line ID not known
    // We infer RAM size based of flash memory size
    switch(flash_size) {
    case 1024:
      axi_sram_size = 384;
      break;
    case 2048:
      axi_sram_size = 512;
      break;
    }
  }

  if(axi_sram_size)
    heap_add_mem(0x24000000, 0x24000000 + axi_sram_size * 1024,
                 MEM_TYPE_DMA);

  // DTCM
  void *DTCM_end   = (void *)0x20000000 + 128 * 1024;
  heap_add_mem(HEAP_START_EBSS, (long)DTCM_end, 0);

  // Packet buffers from SRAM2
  //  pbuf_data_add((void *)0x30020000, (void *)0x30020000 + 32 * 1024);
}
