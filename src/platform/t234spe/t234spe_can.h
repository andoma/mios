#pragma once

#include <stdint.h>

struct dsig_filter;
struct can_netif;

struct can_netif *tegra243_spe_can_init(unsigned int nominal_bitrate,
                                        unsigned int data_bitrate,
                                        const struct dsig_filter *input_filter,
                                        const struct dsig_filter *output_filter,
                                        uint32_t flags, const char *name);
