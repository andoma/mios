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
  gpio_set_output(GPIO_D, 13, 1);
  uart_irq(&console);
  gpio_set_output(GPIO_D, 13, 0);
}


static void __attribute__((constructor(110)))
console_init_early(void)
{
  reg_set(RCC_AHB1ENR, 0x01);    // CLK ENABLE: GPIOA
  reg_set(RCC_APB1ENR, 0x20000); // CLK ENABLE: USART2

  // Configure PA2 for USART2 TX (Alternative Function 7)
  gpio_conf_af(GPIO_A, 2, 7, GPIO_SPEED_HIGH, GPIO_PULL_NONE);
  gpio_conf_af(GPIO_A, 3, 7, GPIO_SPEED_HIGH, GPIO_PULL_UP);

  uart_init(&console, 0x40004400, 115200);

  irq_enable(38, IRQ_LEVEL_CONSOLE);

  init_printf(&console, uart_putc);
  init_getchar(&console, uart_getc);
  uart_putc(&console, '\n');
  uart_putc(&console, '\n');
  uart_putc(&console, '\n');
}

