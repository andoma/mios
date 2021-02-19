#include <assert.h>
#include <stdio.h>
#include <unistd.h>

#include <mios/io.h>
#include <mios/mios.h>
#include <mios/task.h>

#include "irq.h"

#include "stm32f4.h"
#include "stm32f4_clk.h"
#include "stm32f4_uart.h"

static stm32f4_uart_t console;

static void __attribute__((constructor(110)))
board_init_console(void)
{
  clk_enable(CLK_GPIOA);

  stm32f4_uart_init(&console, 2, 115200, GPIO_PA(2), GPIO_PA(3),
                    UART_CTRLD_IS_PANIC);
  stdio = &console.stream;
}






static void __attribute__((constructor(101)))
board_setup_clocks(void)
{
  reg_wr(FLASH_ACR, 0x75); // D-CACHE I-CACHE PREFETCH, 5 wait states

  reg_wr(RCC_CFGR,
         (0x7 << 27) | // MCO2PRE /5
         (0x4 << 13) | // APB2 (High speed) prescaler = 2
         (0x5 << 10)); // APB1 (Low speed)  prescaler = 4

  reg_set_bit(RCC_CR, 16); // HSEON

  while(!(reg_rd(RCC_CR) & (1 << 17))) {} // Wait for external oscillator

  reg_wr(RCC_PLLCFGR,
         (1 << 22)
         | (4 << 0)         // input division
         | (168 << 6)       // PLL multiplication
         | (0 << 16)        // PLL sys clock division (0 == /2) */
         | (7 << 24));      // PLL usb clock division =48MHz */

  reg_set_bit(RCC_CR, 24);

  while(!(reg_rd(RCC_CR) & (1 << 25))) {} // Wait for pll

  reg_set_bits(RCC_CFGR, 0, 2, 2); // Use PLL as system clock

  while((reg_rd(RCC_CFGR) & 0xc) != 0x8) {}

#if 0
  // Drive PLL output clock / 5 to PC9
  reg_set(RCC_AHB1ENR, 0x04);  // CLK ENABLE: GPIOC
  gpio_conf_af(GPIO_C, 9, 0, GPIO_SPEED_HIGH, GPIO_PULL_UP);
#endif


  clk_enable(CLK_GPIOD);
  for(int i = 0; i < 4; i++) {
    gpio_conf_output(GPIO_PD(i + 12), GPIO_PUSH_PULL,
                     GPIO_SPEED_LOW, GPIO_PULL_NONE);
  }

  gpio_set_output(GPIO_PD(15), 1);
}




static void *
blinker(void *arg)
{
  while(1) {
    gpio_set_output(GPIO_PD(12), 1);
    usleep(250000);
    gpio_set_output(GPIO_PD(12), 0);
    usleep(250000);
  }
  return NULL;
}

static void __attribute__((constructor(800)))
platform_init_late(void)
{
  task_create(blinker, NULL, 512, "blinker", 0, 0);
}

