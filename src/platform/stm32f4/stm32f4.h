#pragma once


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


#define RCC_AHB1ENR 0x40023830
#define RCC_APB1ENR 0x40023840

