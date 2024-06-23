#pragma once

#include <mios/io.h>

void stm32g4_fdcan_init(int instance, gpio_t can_tx, gpio_t can_rx);

