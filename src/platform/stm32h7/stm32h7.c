#include <stdint.h>
#include <stdio.h>
#include <malloc.h>

#include <mios/sys.h>
#include <mios/cli.h>

#include <net/pbuf.h>

#include "stm32h7_clk.h"
#include "cache.h"
#include "mpu.h"
#include "irq.h"

static volatile uint16_t *const FLASH_SIZE   = (volatile uint16_t *)0x1FF1E880;
static volatile uint32_t *const LINE_ID      = (volatile uint32_t *)0x1FF1E8c0;
static volatile uint32_t *const SYSCFG_PKGR  = (volatile uint32_t *)0x58000524;

static void __attribute__((constructor(101)))
enter_dfu(void)
{

}



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
  case 0x48373235: // STM32H725
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
                 MEM_TYPE_VECTOR_TABLE | MEM_TYPE_DMA, 20);

  // DTCM
  void *DTCM_end   = (void *)0x20000000 + 128 * 1024;
  heap_add_mem(HEAP_START_EBSS, (long)DTCM_end,
               MEM_TYPE_VECTOR_TABLE | MEM_TYPE_LOCAL, 10);

  switch(line_id) {
  case 0x48373233: // STM32H723
  case 0x48373235: // STM32H725
    // SRAM1
    mpu_add_region((void *)0x30000000, 14,
                   MPU_NORMAL_NON_SHARED_NON_CACHED | MPU_AP_RW | MPU_XN);
    pbuf_data_add((void *)0x30000000, (void *)0x30004000);

    // SRAM2
    mpu_add_region((void *)0x30004000, 14,
                   MPU_NORMAL_NON_SHARED_NON_CACHED | MPU_AP_RW | MPU_XN);
    heap_add_mem(0x30004000, 0x30008000,
                 MEM_TYPE_DMA | MEM_TYPE_NO_CACHE, 30);

    // SRAM4
    mpu_add_region((void *)0x38000000, 14,
                   MPU_NORMAL_NON_SHARED_NON_CACHED | MPU_AP_RW | MPU_XN);
    heap_add_mem(0x38000000, 0x38004000,
                 MEM_TYPE_DMA | MEM_TYPE_NO_CACHE, 40);
    break;
  }
}


const struct serial_number
sys_get_serial_number(void)
{
  struct serial_number sn;
  sn.data = (const void *)0x1ff1e800;
  sn.len = 12;
  return sn;
}


static error_t
cmd_dfu(cli_t *cli, int argc, char **argv)
{
  irq_forbid(IRQ_LEVEL_ALL);
  mpu_disable();
  fini();
  systick_deinit();
  softreset(0x1ff09800);
}

CLI_CMD_DEF("dfu", cmd_dfu);
