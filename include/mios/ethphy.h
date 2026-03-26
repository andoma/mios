#pragma once

#include <stdint.h>
#include <mios/error.h>
#include <mios/stream.h>
#include <mios/device.h>

typedef enum {
  ETHPHY_MODE_MII,
  ETHPHY_MODE_RMII,
  ETHPHY_MODE_RGMII,
} ethphy_mode_t;

struct ether_netif;

typedef device_t *(ethphy_init_t)(struct ether_netif *mac, ethphy_mode_t mode);

device_t *ethphy_create(device_t *parent, const device_class_t *dc,
                        size_t size);
/**
 * Read & write MII registers from currently attached PHY
 *
 * Handles IEEE 802.3 Clause 22 Extension (Extended register access)
 */

int ethphy_mii_read(struct ether_netif *eni, uint16_t reg);

error_t ethphy_mii_write(struct ether_netif *eni, uint16_t reg, uint16_t value);

/**
 * Poll PHY link status (MII register 1, bit 2) in a loop.
 * Raises NETIF_TASK_STATUS_UP/DOWN on transitions.
 */
void ethphy_link_poll(struct ether_netif *eni) __attribute__((noreturn));

void ethphy_print_status(struct ether_netif *eni, struct stream *s);
