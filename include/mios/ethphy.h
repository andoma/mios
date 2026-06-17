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

// Flags for ethphy_init
#define ETHPHY_DELAY_TX  0x1  // PHY should add TX clock delay
#define ETHPHY_DELAY_RX  0x2  // PHY should add RX clock delay

struct ether_netif;

typedef device_t *(ethphy_init_t)(struct ether_netif *mac, ethphy_mode_t mode,
                                  unsigned int flags);

typedef struct ethphy_device_class {
  device_class_t dc;
  void (*link_poll)(struct ether_netif *eni) __attribute__((noreturn));
} ethphy_device_class_t;

device_t *ethphy_create(device_t *parent, const ethphy_device_class_t *dc,
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

/**
 * Sleep inside a PHY link-poll loop for up to `useconds`, waking early if a
 * shutdown has been requested via ethphy_poll_stop(). If shutdown was
 * requested, this terminates the calling thread (thread_exit) and does not
 * return -- this is how a noreturn link-poll loop is unwound cleanly.
 */
void ethphy_poll_sleep(struct ether_netif *eni, int useconds);

/**
 * Request the PHY link-poll thread for `eni` to terminate, and wake it so it
 * exits immediately rather than after its next poll interval. Pair with
 * thread_join() on the poll thread to ensure no further MII/MMIO access
 * happens (e.g. before powergating the controller).
 */
void ethphy_poll_stop(struct ether_netif *eni);

void ethphy_print_status(struct ether_netif *eni, struct stream *s);
