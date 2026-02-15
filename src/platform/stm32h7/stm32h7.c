#include <stdint.h>
#include <stdio.h>
#include <malloc.h>
#include <string.h>

#include <mios/sys.h>
#include <mios/cli.h>
#include <mios/atomic.h>

#include <net/pbuf.h>

#include "stm32h7_clk.h"
#include "cache.h"
#include "mpu.h"
#include "irq.h"
#include "cpu.h"

#define CRASHLOG_SIZE  512
#define CRASHLOG_ADDR  (0x38004000 - CRASHLOG_SIZE)

static void
get_crashlog_stream_prep(void)
{
}

#include "lib/sys/crashlog.c"


static volatile uint16_t *const FLASH_SIZE   = (volatile uint16_t *)0x1FF1E880;
static volatile uint32_t *const LINE_ID      = (volatile uint32_t *)0x1FF1E8c0;
static volatile uint32_t *const SYSCFG_PKGR  = (volatile uint32_t *)0x58000524;
static volatile uint32_t *const DWT_CONTROL  = (volatile uint32_t *)0xE0001000;



static const char *packages =
  "VFQFPN68\0"
  "LQFP100 Legacy / TFBGA100 Legacy\0"
  "LQFP100\0"
  "TFBGA100\0"
  "WLCSP115\0"
  "LQFP144\0"
  "UFBGA144\0"
  "LQFP144\0"
  "UFBGA169\0"
  "UFBGA176+25\0"
  "LQFP176\0\0";

static uint8_t code_itcm;

static void  __attribute__((constructor(120)))
stm32h7_init(void)
{
  clk_enable(CLK_SYSCFG);

  uint32_t pkg = *SYSCFG_PKGR & 0xf;
  const char *pkgstr = strtbl(packages, pkg);

  const int flash_size = *FLASH_SIZE;
  const uint32_t line_id = *LINE_ID;

  printf("\nSTM32%c%c%c%c (%s) (%d kB Flash)\n",
         line_id >> 24,
         line_id >> 16,
         line_id >> 8,
         line_id,
         pkgstr ?: "???", flash_size);


  crashlog_recover();

  long axi_sram_size = 0;

  switch(line_id) {
  case 0x48373233: // STM32H723
  case 0x48373235: // STM32H725
    heap_add_mem(0x1000, 0x00010000,
                 MEM_TYPE_CODE | MEM_TYPE_VECTOR_TABLE, 40);

    // This actually depends on TCM_AXI_SHARED option bits
    axi_sram_size = 320;
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
                 MEM_TYPE_DMA, 20);

  // DTCM
  void *DTCM_end   = (void *)0x20000000 + 128 * 1024;
  heap_add_mem(HEAP_START_EBSS, (long)DTCM_end,
               MEM_TYPE_LOCAL, 10);

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
    heap_add_mem(0x38000000, CRASHLOG_ADDR,
                 MEM_TYPE_DMA | MEM_TYPE_NO_CACHE, 40);
    break;
  }


  code_itcm = mpu_add_region(NULL, 16, MPU_AP_RO);

  mpu_add_region(NULL, 12, 0); // Map 0 - 4095 as no-access at all times
  *DWT_CONTROL = 1;

}

static atomic_t code_unprotect_counter;

void
mpu_protect_code(int on)
{
  int n = atomic_add_and_fetch(&code_unprotect_counter, on ? -1 : 1);
  mpu_set_region(NULL, 16, n ? MPU_AP_RW : MPU_AP_RO, code_itcm);
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
  shutdown_notification("DFU");
  irq_forbid(IRQ_LEVEL_ALL);
  mpu_disable();
  fini();
  stm32h7_clk_deinit();
  softreset(0x1ff09800);
}

CLI_CMD_DEF_EXT("dfu", cmd_dfu, NULL, "Enter Device Firmware Upgrade");
