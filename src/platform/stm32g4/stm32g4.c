#include <stdint.h>
#include <stdio.h>
#include <malloc.h>

#include <net/pbuf.h>

#include "cpu.h"
#include "irq.h"

#include "stm32g4_clk.h"
#include "stm32g4_usb.h"

static volatile uint32_t *const DWT_CONTROL  = (volatile uint32_t *)0xE0001000;
static volatile uint32_t *const DWT_LAR      = (volatile uint32_t *)0xE0001FB0;
static volatile uint32_t *const SCB_DEMCR    = (volatile uint32_t *)0xE000EDFC;

static volatile uint16_t *const FLASH_SIZE = (volatile uint16_t *)0x1fff75e0;
static volatile uint32_t *const DBGMCU_IDCODE = (volatile uint32_t *)0xe0042000;
static volatile uint32_t *const SYSCFG_MEMRMP = (volatile uint32_t *)0x40010000;

static struct {
  uint16_t id;
  uint8_t sram1_size;
  uint8_t sram2_size;
  uint8_t ccm_size;
  char category;
} devicemap[] = {
  { 0x468, 16,  6, 10, '2' },
  { 0x469, 80, 16, 16, '3' },
  { 0x479, 80, 16, 16, '4' },
};


static void  __attribute__((constructor(120)))
stm32g4_init(void)
{
  uint32_t chipid = *DBGMCU_IDCODE;
  uint16_t deviceid = chipid & 0xfff;

  uint8_t sram1_size = 16;
  uint8_t sram2_size = 6;
  uint8_t ccm_size   = 10;
  uint8_t category = '?';

  for(size_t i = 0; i < sizeof(devicemap) / sizeof(devicemap[0]); i++) {
    if(devicemap[i].id == deviceid) {
      sram1_size = devicemap[i].sram1_size;
      sram2_size = devicemap[i].sram2_size;
      ccm_size   = devicemap[i].ccm_size;
      category = devicemap[i].category;
      break;
    }
  }

  printf("\nSTM32G4 (%d kB Flash) Cat:%c (SRAM: %d + %d + %d kB) ID:0x%08x\n",
         *FLASH_SIZE, category, sram1_size, sram2_size, ccm_size, chipid);

  void *SRAM1_end   = (void *)0x20000000 + sram1_size * 1024;
  heap_add_mem(HEAP_START_EBSS, (long)SRAM1_end, MEM_TYPE_DMA);

  void *SRAM2_start = (void *)0x20014000;
  void *SRAM2_end   = (void *)0x20014000 + sram2_size * 1024;
  pbuf_data_add(SRAM2_start, SRAM2_end);

  void *CCM_start = (void *)0x10000000 + sizeof(cpu_t);
  void *CCM_end = (void *)0x10000000 + ccm_size * 1024;
  heap_add_mem((long)CCM_start, (long)CCM_end, MEM_TYPE_LOCAL);

  // Enable cycle counter
  *SCB_DEMCR |= 0x01000000;
  *DWT_LAR = 0xC5ACCE55; // unlock
  *DWT_CONTROL = 1;

}


void
dfu(void)
{
  irq_forbid(IRQ_LEVEL_ALL);
  *SYSCFG_MEMRMP = 1; // Map system flash to 0x0
  stm32g4_usb_stop();
  stm32g4_deinit_clk();
  systick_deinit();
  softreset();
}

