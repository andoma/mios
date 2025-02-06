#pragma once

#include <mios/io.h>

struct dsig_filter;

void stm32g4_fdcan_init(int instance, gpio_t can_tx, gpio_t can_rx,
                        unsigned int nominal_bitrate,
                        unsigned int data_bitrate,
                        const struct dsig_filter *output_filter);

