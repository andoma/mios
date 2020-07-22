#pragma once

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


typedef enum {
  GPIO_PULL_NONE = 0,
  GPIO_PULL_UP = 1,
  GPIO_PULL_DOWN = 2,
} gpio_pull_t;

typedef enum {
  GPIO_PUSH_PULL = 0,
  GPIO_OPEN_DRAIN = 1,
} gpio_output_type_t;


typedef enum {
  GPIO_SPEED_LOW       = 0,
  GPIO_SPEED_MID       = 1,
  GPIO_SPEED_HIGH      = 2,
  GPIO_SPEED_VERY_HIGH = 3,
} gpio_output_speed_t;


void gpio_conf_input(int gpio, gpio_pull_t pull);

void gpio_conf_output(int gpio, gpio_output_type_t type,
                      gpio_output_speed_t speed, gpio_pull_t pull);

void gpio_set_output(int gpio, int on);

void gpio_conf_af(int gpio, int af, gpio_output_type_t type,
                  gpio_output_speed_t speed, gpio_pull_t pull);
