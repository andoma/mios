#include <stdint.h>
#include <stddef.h>
#include <stdio.h>

#include "irq.h"
#include "platform.h"
#include "reg.h"
#include "stm32f4.h"

#include "gpio.h"

//static volatile unsigned int * const GPIOA_MODER   = (unsigned int *)0x40020000;
//static volatile unsigned int * const GPIOA_OSPEEDR = (unsigned int *)0x40020008;
//static volatile unsigned int * const GPIOA_AFRL    = (unsigned int *)0x40020020;

static volatile unsigned int * const USART2_SR     = (unsigned int *)0x40004400;
static volatile unsigned int * const USART2_DR     = (unsigned int *)0x40004404;
static volatile unsigned int * const USART2_BBR    = (unsigned int *)0x40004408;
static volatile unsigned int * const USART2_CR1    = (unsigned int *)0x4000440c;


void *
platform_heap_end(void)
{
  return (void *)0x20000000 + 112 * 1024;
}




static void
uart_putc(void *p, char c)
{
  *USART2_DR = c;
  while(!(*USART2_SR & (1 << 7))) {}
}





void
platform_console_init_early(void)
{
  reg_set(RCC_AHB1ENR, 0x09);  // CLK ENABLE: GPIOA GPIOD
  reg_set(RCC_APB1ENR, 0x20000); // CLK ENABLE: USART2

  for(int i = 0; i < 4; i++) {
    gpio_conf_output(GPIO_D, i + 12, GPIO_PUSH_PULL,
                     GPIO_SPEED_LOW, GPIO_PULL_NONE);
  }


  gpio_set_output(GPIO_D, 15, 1);
  gpio_conf_af(GPIO_A, 2, 7, GPIO_SPEED_HIGH, GPIO_PULL_NONE);

  *USART2_CR1 = (1 << 13);
 // 16000000 / 115200;
  *USART2_BBR = (8 << 4) | 11;
  *USART2_CR1 = (1 << 13) | (1 << 3);

  init_printf(NULL, uart_putc);
}



void
platform_init(void)
{

}
