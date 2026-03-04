#pragma once

#include <stdint.h>
#include <mios/error.h>
#include <mios/device.h>
#include <mios/stream.h>

typedef enum {
  ETHPHY_MODE_MII,
  ETHPHY_MODE_RMII,
  ETHPHY_MODE_RGMII,
} ethphy_mode_t;

typedef struct ethphy_reg_io {
  uint16_t (*read)(void *arg, uint16_t reg);
  void (*write)(void *arg, uint16_t reg, uint16_t value);
} ethphy_reg_io_t;

typedef struct ethphy_dev {
  device_t ed_dev;
  ethphy_mode_t ed_mode;
  const ethphy_reg_io_t *ed_regio;
  void *ed_arg;
} ethphy_dev_t;
