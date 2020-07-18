#include <stdint.h>
#include <stddef.h>
#include <stdio.h>

#include "irq.h"
#include "platform.h"

static volatile unsigned int * const RCC_AHB1ENR = (unsigned int *)0x40023830;
static volatile unsigned int * const RCC_APB1ENR = (unsigned int *)0x40023840;

static volatile unsigned int * const GPIOD_MODER   = (unsigned int *)0x40020c00;
static volatile unsigned int * const GPIOD_OTYPER  = (unsigned int *)0x40020c04;
static volatile unsigned int * const GPIOD_OSPEEDR = (unsigned int *)0x40020c08;
static volatile unsigned int * const GPIOD_PUPDR   = (unsigned int *)0x40020c0c;
static volatile unsigned int * const GPIOD_ODR     = (unsigned int *)0x40020c14;

static volatile unsigned int * const GPIOA_MODER   = (unsigned int *)0x40020000;
//static volatile unsigned int * const GPIOA_OTYPER  = (unsigned int *)0x40020004;
static volatile unsigned int * const GPIOA_OSPEEDR = (unsigned int *)0x40020008;
//static volatile unsigned int * const GPIOA_PUPDR   = (unsigned int *)0x4002000c;
//static volatile unsigned int * const GPIOA_ODR     = (unsigned int *)0x40020014;
static volatile unsigned int * const GPIOA_AFRL    = (unsigned int *)0x40020020;

static volatile unsigned int * const USART2_SR     = (unsigned int *)0x40004400;
static volatile unsigned int * const USART2_DR     = (unsigned int *)0x40004404;
static volatile unsigned int * const USART2_BBR    = (unsigned int *)0x40004408;
static volatile unsigned int * const USART2_CR1    = (unsigned int *)0x4000440c;


void *
platform_heap_end(void)
{
  return (void *)0x20000000 + 112 * 1024;
}




static void  __attribute__((unused))
uart_putc(void *p, char c)
{
  *USART2_DR = c;

  *GPIOD_ODR |= 0x2000;
  while(!(*USART2_SR & (1 << 7))) {}
  *GPIOD_ODR &= ~0x2000;
}

static void __attribute__((unused))
delay(void)
{
  for(int i = 0; i < 50000; i++) {
    asm("");
  }
}


void
platform_panic(void)
{
  *GPIOD_ODR = 0x4000;
}

void
platform_console_init_early(void)
{
  *RCC_AHB1ENR |= 0x9; // CLK ENABLE: GPIOA GPIOD

  *RCC_APB1ENR |= 0x20000; // CLK ENABLE: USART2


  *GPIOD_MODER = (*GPIOD_MODER & 0x00ffffff) | 0x55000000;
  *GPIOD_OTYPER = *GPIOD_OTYPER & 0xffff0fff;
  *GPIOD_OSPEEDR = *GPIOD_OSPEEDR & 0x00ffffff;
  *GPIOD_PUPDR = *GPIOD_PUPDR & 0x00ffffff;

  *GPIOD_ODR = 0x8000;

  *GPIOA_MODER   |= 2 << (2 * 2);
  *GPIOA_OSPEEDR |= 2 << (2 * 2);
  *GPIOA_AFRL    |= 7 << (2 * 4);

  *USART2_CR1 = (1 << 13);
 // 16000000 / 115200;
  *USART2_BBR = (8 << 4) | 11;
  *USART2_CR1 = (1 << 13) | (1 << 3);

  init_printf(NULL, uart_putc);
}



void
platform_init(void)
{
  delay();
  *GPIOD_ODR = 0x0000;
  delay();
  *GPIOD_ODR = 0x1000;
}
