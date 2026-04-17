#pragma once

#include <mios/io.h>
#include <mios/ethphy.h>

struct ether_netif *
stm32n6_eth_init(gpio_t phyrst, gpio_t mdio, gpio_t mdc,
                 int phy_addr, ethphy_mode_t mode);
