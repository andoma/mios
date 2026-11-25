#pragma once

/*
 * Virtual SPI bus, for device models on the host.
 *
 * vspi_create() returns an spi_t that drivers use like any other bus.
 * Device models attach with the GPIO their chip select is wired to.
 *
 * A transfer works like the STM32 driver (stm32_spi.c): the bus drives
 * the nss pin low, clocks the bytes, and drives it high again. The pin is
 * a vgpio.c pin, and a chip takes part in the transfer if its chip
 * select is low. A pin the board never made an output does not go low
 * when the bus drives it, and a floating one (no output, no pull, nothing
 * driving it) selects nothing either, so a chip whose chip select is not
 * set up is not selected and MISO reads 0xff.
 *
 * MISO is open drain with a pull-up: a byte nobody drives reads 0xff,
 * and several selected chips AND their outputs.
 *
 * get_config() returns the SPI_CPOL / SPI_CPHA bits asked for, and that
 * is the mode a model's select() gets.
 *
 * The model callbacks run on the thread doing the transfer, with the
 * bus locked. They must not block or call back into the bus.
 */

#include <stdint.h>
#include <mios/io.h>
#include <mios/error.h>

typedef struct vspi_dev vspi_dev_t;

typedef struct vspi_dev_ops {

  // Chip select went low. mode is SPI_CPOL | SPI_CPHA of the transfer.
  void (*select)(vspi_dev_t *d, int mode);

  // One byte clocked: mosi in, return what the chip shifts out on MISO
  // (0xff for "not driving")
  uint8_t (*xfer)(vspi_dev_t *d, uint8_t mosi);

  // Chip select went high
  void (*deselect)(vspi_dev_t *d);

} vspi_dev_ops_t;

struct vspi_dev {
  const vspi_dev_ops_t *ops;
};

spi_t *vspi_create(const char *name);

// ERR_EXIST if something is attached to cs already
error_t vspi_attach(spi_t *bus, gpio_t cs, vspi_dev_t *dev);

void vspi_detach(spi_t *bus, gpio_t cs);
