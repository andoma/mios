#pragma once

// Included by io.h

#define GPIO(PORT, BIT) (((PORT) << 4) | (BIT))

#define GPIO_PA(x)  GPIO(0, x)
#define GPIO_PB(x)  GPIO(1, x)
#define GPIO_PC(x)  GPIO(2, x)
#define GPIO_PD(x)  GPIO(3, x)
#define GPIO_PE(x)  GPIO(4, x)
#define GPIO_PF(x)  GPIO(5, x)
#define GPIO_PG(x)  GPIO(6, x)
#define GPIO_PH(x)  GPIO(7, x)
#define GPIO_PI(x)  GPIO(8, x)
#define GPIO_PJ(x)  GPIO(9, x)
#define GPIO_PK(x)  GPIO(10, x)

typedef unsigned char gpio_t;

void gpio_conf_af(gpio_t gpio, int af, gpio_output_type_t type,
                  gpio_output_speed_t speed, gpio_pull_t pull);
