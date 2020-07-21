#pragma once

#define GPIO_A 0
#define GPIO_B 1
#define GPIO_C 2
#define GPIO_D 3
#define GPIO_E 4
#define GPIO_F 5
#define GPIO_G 6
#define GPIO_H 7
#define GPIO_I 8
#define GPIO_J 9
#define GPIO_K 10



#define GPIO_PORT(x) (0x40020000 + ((x) * 0x400))

#define GPIO_MODER(x)   (GPIO_PORT(x) + 0x00)
#define GPIO_OTYPER(x)  (GPIO_PORT(x) + 0x04)
#define GPIO_OSPEEDR(x) (GPIO_PORT(x) + 0x08)
#define GPIO_PUPDR(x)   (GPIO_PORT(x) + 0x0c)
#define GPIO_IDR(x)     (GPIO_PORT(x) + 0x10)
#define GPIO_ODR(x)     (GPIO_PORT(x) + 0x14)
#define GPIO_BSRR(x)    (GPIO_PORT(x) + 0x18)
#define GPIO_LCKR(x)    (GPIO_PORT(x) + 0x1c)
#define GPIO_AFRL(x)    (GPIO_PORT(x) + 0x20)
#define GPIO_AFRH(x)    (GPIO_PORT(x) + 0x24)




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




void gpio_conf_input(int port, int bit, gpio_pull_t pull);

void gpio_conf_output(int port, int bit, gpio_output_type_t type,
                      gpio_output_speed_t speed, gpio_pull_t pull);

void gpio_set_output(int port, int bit, int on);

void gpio_conf_af(int port, int bit, int af, gpio_output_speed_t speed,
                  gpio_pull_t pull);
