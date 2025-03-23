#pragma once

#include <mios/io.h>

struct dsig_filter;

struct can_netif;

struct can_netif* stm32f4_can_init(int instance, gpio_t txd, gpio_t rxd,
                                   int bitrate,
                                   const struct dsig_filter *input_filter,
                                   const struct dsig_filter *output_filter);
