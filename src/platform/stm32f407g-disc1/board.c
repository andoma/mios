#include <assert.h>
#include <stdio.h>

#include "mios.h"
#include "irq.h"
#include "task.h"

#include "stm32f4.h"
#include "gpio.h"

#include "uart.h"

static uart_t console;

void
irq_38(void)
{
  gpio_set_output(GPIO_PD(13), 1);
  uart_irq(&console);
  gpio_set_output(GPIO_PD(13), 0);
}


static void __attribute__((constructor(110)))
board_init_console(void)
{
  reg_set(RCC_AHB1ENR, 0x01);    // CLK ENABLE: GPIOA
  reg_set(RCC_APB1ENR, 0x20000); // CLK ENABLE: USART2

  // Configure PA2 for USART2 TX (Alternative Function 7)
  gpio_conf_af(GPIO_PA(2), 7,
               GPIO_PUSH_PULL, GPIO_SPEED_HIGH, GPIO_PULL_NONE);
  gpio_conf_af(GPIO_PA(3), 7,
               GPIO_PUSH_PULL, GPIO_SPEED_HIGH, GPIO_PULL_UP);

  uart_init(&console, 0x40004400, 115200);

  irq_enable(38, IRQ_LEVEL_CONSOLE);

  init_printf(&console, uart_putc);
  init_getchar(&console, uart_getc);
}






static void __attribute__((constructor(101)))
board_setup_clocks(void)
{
  reg_wr(FLASH_ACR, 0x75); // D-CACHE I-CACHE PREFETCH, 5 wait states

  reg_wr(RCC_CFGR,
         (0x7 << 27) | // MCO2PRE /5
         (0x4 << 13) | // APB2 (High speed) prescaler = 2
         (0x5 << 10)); // APB1 (Low speed)  prescaler = 4

  reg_set(RCC_CR, 1 << 16); // HSEON

  while(!(reg_rd(RCC_CR) & (1 << 17))) {} // Wait for external oscillator

  reg_wr(RCC_PLLCFGR,
         (1 << 22)
         | (4 << 0)         // input division
         | (168 << 6)       // PLL multiplication
         | (0 << 16)        // PLL sys clock division (0 == /2) */
         | (7 << 24));      // PLL usb clock division =48MHz */

  reg_set(RCC_CR, 1 << 24);

  while(!(reg_rd(RCC_CR) & (1 << 25))) {} // Wait for pll

  reg_set_bits(RCC_CFGR, 0, 2, 2); // Use PLL as system clock

  while((reg_rd(RCC_CFGR) & 0xc) != 0x8) {}

#if 0
  // Drive PLL output clock / 5 to PC9
  reg_set(RCC_AHB1ENR, 0x04);  // CLK ENABLE: GPIOC
  gpio_conf_af(GPIO_C, 9, 0, GPIO_SPEED_HIGH, GPIO_PULL_UP);
#endif


  reg_set(RCC_AHB1ENR, 0x08);  // CLK ENABLE: GPIOD
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
    sleephz(HZ / 2);
    gpio_set_output(GPIO_PD(12), 0);
    sleephz(HZ / 2);
  }
  return NULL;
}

static void __attribute__((constructor(800)))
platform_init_late(void)
{
  task_create(blinker, NULL, 256, "blinker", 0);
}

