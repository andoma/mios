#pragma once

#include <mios/io.h>
#include <mios/ethphy.h>

void stm32n6_eth_init(gpio_t phyrst, int phy_addr, ethphy_mode_t mode);
