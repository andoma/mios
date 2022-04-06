#pragma once

// Included by io.h

#define GPIO(PORT, BIT) (((PORT) << 4) | (BIT))

#define GPIO_PA(x)  GPIO(0, x)
#define GPIO_PB(x)  GPIO(1, x)
#define GPIO_PC(x)  GPIO(2, x)
#define GPIO_PE(x)  GPIO(4, x)
#define GPIO_PH(x)  GPIO(7, x)

#define GPIO_UNUSED 0xff

typedef unsigned char gpio_t;

void gpio_conf_af(gpio_t gpio, int af, gpio_output_type_t type,
                  gpio_output_speed_t speed, gpio_pull_t pull);
