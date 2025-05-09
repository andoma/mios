#pragma once

#include <mios/io.h>

struct dsig_filter;
struct can_netif;

#define STM32H7_CAN_TIM3_TIMESTAMPING 0x1

struct can_netif *stm32h7_can_init(int instance, gpio_t can_tx, gpio_t can_rx,
                                   unsigned int nominal_bitrate,
                                   unsigned int data_bitrate,
                                   const struct dsig_filter *input_filter,
                                   const struct dsig_filter *output_filter,
                                   uint32_t flags, const char *name);

