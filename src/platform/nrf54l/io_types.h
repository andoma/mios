#pragma once

// gpio_t encodes the pin as (port << 5) | pin, which matches the
// nRF54L PSEL register layout (PIN[4:0], PORT[7:5]) directly.

#define GPIO_P0(x)  (x)
#define GPIO_P1(x)  ((1 << 5) | (x))
#define GPIO_P2(x)  ((2 << 5) | (x))

#define GPIO_UNUSED 0xff

typedef unsigned char gpio_t;
