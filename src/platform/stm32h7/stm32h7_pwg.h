#pragma once

#include <mios/pwg.h>

typedef struct stm32_pwg stm32_pwg_t;

stm32_pwg_t *stm32h7_pwg_create(unsigned int timer_instance,
                                 uint8_t gpio_port,
                                 uint32_t frequency,
                                 pwg_fill_cb fill_cb,
                                 void *opaque);

void stm32_pwg_stop(stm32_pwg_t *pwg);
