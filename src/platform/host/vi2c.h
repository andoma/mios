#pragma once

/*
 * Virtual I2C bus, for device models on the host.
 *
 * vi2c_create() returns an i2c_t that drivers use like any other bus.
 * Device models attach at their 7-bit address and see the transfer the
 * way a target on the wire does: a START (or repeated START) with the
 * direction, the bytes, and the STOP. A transfer to an address nothing
 * is attached to is a NACK of the address.
 *
 * One i2c_t rwv() call is one transfer: if there are tx segments, a
 * START for write and the tx bytes of all segments in order; if there
 * are rx segments, a (repeated) START for read and the rx segments
 * filled in order; then a STOP. This is how the STM32 drivers run it.
 *
 * Results follow the STM32F4 driver (stm32_i2c_v1.c): an address NACK is
 * ERR_NO_DEVICE, a data NACK ERR_BUS_ERROR.
 *
 * The model callbacks run on the thread doing the transfer, with the
 * bus's mutex held. They must not block or call back into the bus.
 */

#include <stdint.h>
#include <mios/io.h>
#include <mios/error.h>

typedef struct vi2c_dev vi2c_dev_t;

typedef struct vi2c_dev_ops {

  // A START or repeated START to addr (the address the model was
  // attached at), read = 1 for a read. Return 0 to ACK the address,
  // non-zero to NACK it.
  int (*start)(vi2c_dev_t *d, uint8_t addr, int read);

  // A byte written by the controller. Return 0 to ACK, non-zero to NACK
  // (the controller then ends the transfer).
  int (*write)(vi2c_dev_t *d, uint8_t byte);

  // A byte read by the controller
  uint8_t (*read)(vi2c_dev_t *d);

  // STOP
  void (*stop)(vi2c_dev_t *d);

} vi2c_dev_ops_t;

struct vi2c_dev {
  const vi2c_dev_ops_t *ops;
};

i2c_t *vi2c_create(const char *name);

// ERR_EXIST if something is attached there already
error_t vi2c_attach(i2c_t *bus, uint8_t addr, vi2c_dev_t *dev);

void vi2c_detach(i2c_t *bus, uint8_t addr);

// Hold the bus (acquire = 1) or let it go: a model's test API takes it
// around changes to state its callbacks read
void vi2c_lock(i2c_t *bus, int acquire);

// Fail every transfer to addr with err without touching the device, as a
// controller stuck on a held SDA line does (ERR_TIMEOUT). 0 restores.
void vi2c_set_error(i2c_t *bus, uint8_t addr, error_t err);
