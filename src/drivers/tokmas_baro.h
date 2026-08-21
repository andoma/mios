#pragma once

#include <mios/io.h>

struct stream;

// Despite the silkscreen/BOM name, the part actually fitted (TOKMAS
// "BMP388") does NOT share Bosch's BMP388 register map. It's a
// register-for-register clone of the Infineon DPS310/DPS368 family
// instead (confirmed against tokmas_baro.pdf) -- pin-compatible only.
typedef struct tokmas_baro tokmas_baro_t;

// bus/address setup only, does not touch the chip
tokmas_baro_t *tokmas_baro_create(i2c_t *bus, uint8_t i2c_addr);

// Soft-resets the chip, verifies the ID register, reads calibration
// coefficients and starts continuous background pressure+temperature
// measurement at 50Hz (8x pressure oversampling, 1x temperature).
error_t tokmas_baro_reset(tokmas_baro_t *dev);

// Returns ERR_NOT_READY if no new sample has completed since the last read.
error_t tokmas_baro_read(tokmas_baro_t *dev, float *pressure_pa, float *temperature_c);

void tokmas_baro_dump(tokmas_baro_t *dev, struct stream *st);
