#pragma once

// No GPIO on a host process. Type exists so the generic io.h compiles.
typedef int gpio_t;

#define GPIO_UNUSED -1
