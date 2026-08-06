#pragma once

// SX1280 driver internals, shared between transport and modem layers

#include "sx1280.h"
#include "sx1280_regs.h"

#include <mios/task.h>

struct sx1280 {
  spi_t *bus;
  mutex_t mutex;

  task_waitable_t busy_waitq;
  task_waitable_t irq_waitq;
  uint8_t irq_pending;

  gpio_t nss;
  gpio_t nreset;
  gpio_t busy;
  gpio_t dio1;

  int spicfg;

  const char *name;
};

error_t sx1280_cmd(sx1280_t *s, const uint8_t *tx, uint8_t *rx, size_t len);

// Wait for DIO1 to fire, then read-and-clear the chip's IRQ status.
// Returns IRQ bits, 0 on DIO1 timeout, or negative error.
int sx1280_wait_irq(sx1280_t *s, int timeout);
