#pragma once

#include <mios/io.h>

void stm32f4_eth_init(gpio_t phyrst, const uint8_t *gpios, size_t gpio_count);

__attribute__((unused))
static const uint8_t stm32f4_eth_rmii_100pin[] =
  { GPIO_PA(1), GPIO_PA(2), GPIO_PA(7), GPIO_PB(11), GPIO_PB(12),
    GPIO_PB(13), GPIO_PC(1), GPIO_PC(4), GPIO_PC(5)
  };
