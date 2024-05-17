#pragma once

#define GPIO_BASE 0x50000000

#define GPIO_PIN_CNF(x) (GPIO_BASE + 0x700 + (x) * 4)

#define GPIO_OUTSET (GPIO_BASE + 0x508)
#define GPIO_OUTCLR (GPIO_BASE + 0x50c)
#define GPIO_IN     (GPIO_BASE + 0x510)
