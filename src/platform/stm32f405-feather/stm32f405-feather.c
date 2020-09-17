#include <unistd.h>
#include <assert.h>
#include <stdio.h>
#include <io.h>

#include "mios.h"
#include "irq.h"
#include "task.h"

#include "stm32f4.h"
#include "stm32f4_i2c.h"

#include "uart.h"

#define BLINK_GPIO GPIO_PC(1) // Red led close to USB connection

static uart_t console;

void
irq_71(void)
{
  uart_irq(&console);
}


static void __attribute__((constructor(110)))
board_init_console(void)
{
  reg_set_bit(RCC_APB2ENR, 5); // CLK ENABLE: USART6

  // Configure PA2 for USART6 TX (Alternative Function 8)
  gpio_conf_af(GPIO_PC(6), 8,
               GPIO_PUSH_PULL, GPIO_SPEED_HIGH, GPIO_PULL_NONE);

  gpio_conf_af(GPIO_PC(7), 8,
               GPIO_PUSH_PULL, GPIO_SPEED_HIGH, GPIO_PULL_UP);

  uart_init(&console, 0x40011400, 115200 / 2);

  irq_enable(71, IRQ_LEVEL_CONSOLE);

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

  reg_set_bit(RCC_CR, 16); // HSEON

  while(!(reg_rd(RCC_CR) & (1 << 17))) {} // Wait for external oscillator

  reg_wr(RCC_PLLCFGR,
         (1 << 22)
         | (6 << 0)         // input division (12MHz external xtal)
         | (168 << 6)       // PLL multiplication
         | (0 << 16)        // PLL sys clock division (0 == /2) */
         | (7 << 24));      // PLL usb clock division =48MHz */

  reg_set_bit(RCC_CR, 24);

  while(!(reg_rd(RCC_CR) & (1 << 25))) {} // Wait for pll

  reg_set_bits(RCC_CFGR, 0, 2, 2); // Use PLL as system clock

  while((reg_rd(RCC_CFGR) & 0xc) != 0x8) {}


  reg_set_bit(RCC_APB2ENR, 14); // CLK ENABLE: SYSCFG

  reg_set(RCC_AHB1ENR, 0x07);    // CLK ENABLE: GPIOA,B,C

  gpio_conf_output(BLINK_GPIO, GPIO_PUSH_PULL,
                   GPIO_SPEED_LOW, GPIO_PULL_NONE);

  gpio_set_output(BLINK_GPIO, 1); // Red LED
}


static void *
blinker(void *arg)
{
  while(1) {
    usleep(500000);
    gpio_set_output(BLINK_GPIO, 0); // Red LED
    usleep(500000);
    gpio_set_output(BLINK_GPIO, 1); // Red LED
  }
  return NULL;
}

static void __attribute__((constructor(800)))
platform_init_late(void)
{
  task_create(blinker, NULL, 512, "blinker", 0);
}

