#pragma once

/*
 * A generic register-file chip: 256 bytes of registers behind a vi2c.c or
 * vspi.c bus, for the many sensors and expanders whose wire protocol is
 * "address a register, then stream consecutive registers".
 *
 * I2C: the first byte of a write sets the register pointer, the bytes
 * after it are stored at consecutive registers; a read returns
 * consecutive registers from the pointer. The pointer stays where it is
 * between transfers, as on such chips.
 *
 * SPI (ICM-42688, MPU9250, BMI, LSM... convention): the first byte of a
 * transfer is the register, bit 7 set for a read. A read then clocks out
 * consecutive registers, a write stores the following bytes to them.
 * MISO is 0 during the command byte and during a write.
 *
 * The pointer wraps at 256 on both.
 *
 * Nothing changes a register on its own: what a live chip does by itself
 * (a data-ready bit, a measurement) is for whoever owns the model to
 * write into regs[]. The struct is public so it can be allocated where
 * that owner needs it, platform/hostlib keeps its files in .data so a
 * harness can preset registers before boot.
 *
 * A file is attached to one bus at a time.
 */

#include <stdint.h>
#include <mios/io.h>

#include "vi2c.h"
#include "vspi.h"

typedef struct vregfile {
  uint8_t regs[256];

  // Private
  union {
    vi2c_dev_t i2c;
    vspi_dev_t spi;
  } dev;
  uint8_t ptr;
  uint8_t state;
} vregfile_t;

error_t vregfile_attach_i2c(vregfile_t *rf, i2c_t *bus, uint8_t addr);

error_t vregfile_attach_spi(vregfile_t *rf, spi_t *bus, gpio_t cs);
