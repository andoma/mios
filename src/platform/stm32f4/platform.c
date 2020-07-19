#include <stdint.h>
#include <stddef.h>
#include <stdio.h>

#include "irq.h"
#include "platform.h"
#include "reg.h"
#include "stm32f4.h"

#include "gpio.h"
#include "task.h"
#include "mios.h"


void *
platform_heap_end(void)
{
  return (void *)0x20000000 + 112 * 1024;
}



static void __attribute__((constructor(101)))
platform_init_early(void)
{
  reg_set(RCC_AHB1ENR, 0x09);  // CLK ENABLE: GPIOA GPIOD
  reg_set(RCC_APB1ENR, 0x20000); // CLK ENABLE: USART2

  for(int i = 0; i < 4; i++) {
    gpio_conf_output(GPIO_D, i + 12, GPIO_PUSH_PULL,
                     GPIO_SPEED_LOW, GPIO_PULL_NONE);
  }

  gpio_set_output(GPIO_D, 15, 1);
}





static void *
blinker(void *arg)
{
  while(1) {
    gpio_set_output(GPIO_D, 12, 1);
    sleephz(HZ / 2);
    gpio_set_output(GPIO_D, 12, 0);
    sleephz(HZ / 2);
  }
  return NULL;
}

static void __attribute__((constructor(800)))
platform_init_late(void)
{
  task_create(blinker, NULL, 256, "blinker");
}

