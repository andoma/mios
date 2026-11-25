#pragma once

// Included by io.h

// Pins of a host process exist only as state in platform/host/vgpio.c,
// which models and test suites drive and observe. They are numbered the
// way the STM32 ports number theirs, so board code written against STM32
// pin names (GPIO_PB(3)) builds for the host unchanged.

typedef int gpio_t;

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

// Pins 0 .. HOST_GPIO_COUNT - 1 exist
#define HOST_GPIO_COUNT 256

#define GPIO_UNUSED -1
