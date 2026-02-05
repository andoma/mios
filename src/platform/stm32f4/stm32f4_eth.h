#pragma once

#include <mios/io.h>
#include <mios/ethphy.h>


#ifdef ENABLE_NET_PTP
#define STM32F4_ETH_ENABLE_PTP_TIMESTAMPING 0x1
#endif

void stm32f4_eth_init(gpio_t phyrst, const uint8_t *gpios, size_t gpio_count,
                      const ethphy_driver_t *ethphy, int phy_addr,
                      ethphy_mode_t mode, uint32_t flags);


__attribute__((unused))
static const uint8_t stm32f4_eth_rmii_100pin[] =
  { GPIO_PA(1), GPIO_PA(2), GPIO_PA(7), GPIO_PB(11), GPIO_PB(12),
    GPIO_PB(13), GPIO_PC(1), GPIO_PC(4), GPIO_PC(5)
  };

__attribute__((unused))
static const uint8_t stm32f4_eth_mii_100pin[] =
  { GPIO_PA(0), GPIO_PA(1), GPIO_PA(2), GPIO_PA(3), GPIO_PA(7),
    GPIO_PB(0), GPIO_PB(1), GPIO_PB(8), GPIO_PB(10), GPIO_PB(11), GPIO_PB(12),
    GPIO_PB(13),
    GPIO_PC(1), GPIO_PC(2), GPIO_PC(3), GPIO_PC(4), GPIO_PC(5)
  };

int64_t stm32f4_get_current_mac_time(void);
