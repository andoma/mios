#pragma once

// SX1280 driver internals, shared between transport and modem layers

#include "sx1280.h"
#include "sx1280_regs.h"

#include <mios/task.h>
#include <sys/queue.h>

LIST_HEAD(sx1280_slot_list, sx1280_slot);

struct sx1280 {
  spi_t *bus;
  mutex_t mutex;

  task_waitable_t busy_waitq;
  task_waitable_t irq_waitq;
  uint8_t irq_pending;
  uint8_t txdone_pending;

  gpio_t nss;
  gpio_t nreset;
  gpio_t busy;
  gpio_t dio1;
  gpio_t dio_txdone; // Optional; needs a pin whose EXTI line is free
               // (not 3=BUSY, not 15=DIO1)

  int spicfg;

  const char *name;

  // Radio arbiter (sx1280_sched.c)
  mutex_t sched_mutex;
  cond_t sched_cond;
  struct sx1280_slot_list sched_slots;
  const void *sched_mode;
  const struct sx1280_slot *sched_cancelled; // Cancelled while executing
};

void sx1280_sched_init(sx1280_t *s);

error_t sx1280_cmd(sx1280_t *s, const uint8_t *tx, uint8_t *rx, size_t len);

// Wait for DIO1 to fire, then read-and-clear the chip's IRQ status.
// Returns IRQ bits, 0 on DIO1 timeout, or negative error.
int sx1280_wait_irq(sx1280_t *s, int timeout);

// Sleep until DIO1 asserts (absolute deadline); does not read or
// clear the chip's IRQ status. 1 = asserted, 0 = deadline.
int sx1280_wait_dio1(sx1280_t *s, int64_t deadline);

// Same for the TX_DONE pin (chip DIO3 on this board)
int sx1280_wait_txdone_pin(sx1280_t *s, int64_t deadline);

// Read-and-clear the chip's IRQ status (clears only the bits read).
// Returns the bits or negative error.
int sx1280_irq_ack(sx1280_t *s);

// Clear all chip IRQ status and stale software edge flags
error_t sx1280_irq_clear(sx1280_t *s);
