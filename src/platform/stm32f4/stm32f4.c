#include <stdio.h>
#include <malloc.h>

#include <mios/type_macros.h>
#include <mios/cli.h>

#include "stm32f4_clk.h"
#include "stm32f4_reg.h"
#include "cpu.h"
#include "irq.h"
#include "mpu.h"

#include <net/pbuf.h>


#define IWDG_KR  0x40003000
#define IWDG_PR  0x40003004
#define IWDG_RLR 0x40003008
#define IWDG_SR  0x4000300c


static volatile uint16_t *const FLASH_SIZE   = (volatile uint16_t *)0x1fff7a22;
static volatile uint32_t *const ACTLR        = (volatile uint32_t *)0xe000e008;
static volatile uint32_t *const DWT_CONTROL  = (volatile uint32_t *)0xE0001000;
static volatile uint32_t *const DWT_LAR      = (volatile uint32_t *)0xE0001FB0;
static volatile uint32_t *const SCB_DEMCR    = (volatile uint32_t *)0xE000EDFC;

static volatile uint32_t *const DBGMCU_CR    = (volatile uint32_t *)0xe0042004;

static volatile uint32_t *const DBGMCU_IDCODE= (volatile uint32_t *)0xe0042000;

uint32_t stm32f4_device_mask;

typedef struct {
  uint16_t idcode;
  uint8_t bit;
  uint8_t flags;
  char name[4];
} stm32f4_idmap_t;

#define HAVE_CCM 0x1

static const stm32f4_idmap_t stm32f4_idmap[] = {
  {STM32F4_DEVID_05, STM32F4_BIT_05, HAVE_CCM, "05"},
  {STM32F4_DEVID_42, STM32F4_BIT_42, 0, "42"},
  {STM32F4_DEVID_11, STM32F4_BIT_11, 0, "11"},
  {STM32F4_DEVID_46, STM32F4_BIT_46, 0, "46"}
};


static void  __attribute__((constructor(120)))
stm32f4_init(void)
{
  void *SRAM1_end   = (void *)0x20000000 + 112 * 1024;

  uint32_t idcode = *DBGMCU_IDCODE;

  uint32_t dev = idcode & 0xfff;
  const char *name = "??";
  for(size_t i = 0; i < ARRAYSIZE(stm32f4_idmap); i++) {
    if(stm32f4_idmap[i].idcode == dev) {
      name = stm32f4_idmap[i].name;
      stm32f4_device_mask = (1 << stm32f4_idmap[i].bit);
      break;
    }
  }

  printf("\nSTM32F4%s (0x%x, %d kB Flash)\n", name, idcode, *FLASH_SIZE);

  // SRAM1
  heap_add_mem(HEAP_START_EBSS, (long)SRAM1_end,
               MEM_TYPE_DMA | MEM_TYPE_VECTOR_TABLE| MEM_TYPE_CODE, 10);

  pbuf_data_add((void *)0x20000000 + 112 * 1024,
                (void *)0x20000000 + 128 * 1024);


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

  mpu_add_region(NULL, 27, MPU_XN);
}

static void
wdog_init(void)
{
  reg_wr(IWDG_KR, 0x5555);
  reg_wr(IWDG_RLR, 256); // 2 seconds
  reg_wr(IWDG_PR, 6);
  reg_wr(IWDG_KR, 0xAAAA);
  reg_wr(IWDG_KR, 0xCCCC);
}


void  __attribute__((noreturn))
cpu_idle(void)
{
  wdog_init();
  while(1) {
    for(int i = 0; i < 100; i++) {
      asm volatile ("wfi;nop;nop;nop");
    }
    *DBGMCU_CR |= 1;
    reg_wr(IWDG_KR, 0xAAAA);
  }
}


static volatile uint32_t *const SYSCFG_MEMRMP = (volatile uint32_t *)0x40013800;

static error_t
cmd_dfu(cli_t *cli, int argc, char **argv)
{
  shutdown_notification("DFU");
  irq_forbid(IRQ_LEVEL_ALL);
  mpu_disable();

  fini();
  stm32f4_clk_deinit();
  *SYSCFG_MEMRMP = 1; // Map system flash to 0x0
  softreset(0);
}

CLI_CMD_DEF("dfu", cmd_dfu);
