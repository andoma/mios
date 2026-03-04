#pragma once

#include <stdint.h>
#include <mios/error.h>
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

typedef struct {

  error_t (*init)(ethphy_mode_t mode,
                  const ethphy_reg_io_t *regio,
                  void *arg);

  void (*print_diagnostics)(stream_t *s,
                            const ethphy_reg_io_t *regio,
                            void *arg);

} ethphy_driver_t;

extern const ethphy_driver_t ethphy_dp83826;
extern const ethphy_driver_t ethphy_dp83869;
