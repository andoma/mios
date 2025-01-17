#pragma once

#include <mios/io.h>

struct dsig_filter;

void stm32g4_fdcan_init(int instance, gpio_t can_tx, gpio_t can_rx,
                        const struct dsig_filter *output_filter);

