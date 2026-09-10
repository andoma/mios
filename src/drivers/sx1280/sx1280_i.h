#pragma once

// SX1280 driver internals, shared between transport and modem layers

#include "sx1280.h"
#include "sx1280_regs.h"

#include <mios/task.h>
#include <mios/device.h>
#include <sys/queue.h>

LIST_HEAD(sx1280_slot_list, sx1280_slot);

struct sx1280 {
  device_t dev; // must be first (device_t * casts back to sx1280_t *)
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

  // The last commands sent, for the post-mortem of a chip that stopped
  // answering: which command it was in and what preceded it. Written
  // by sx1280_cmd() only.
#define SX1280_CMD_TRACE 32
  struct {
    uint32_t t_us;     // clock_get() low 32 bits, at the SPI transfer
    uint8_t opcode;
    uint8_t param0;    // first parameter byte, 0 if none
    uint8_t status;    // status byte the chip shifted out, if read back
    uint8_t wait_ms;   // BUSY wait before the transfer, 255 = timed out
  } cmd_trace[SX1280_CMD_TRACE];
  uint8_t cmd_trace_wr;

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
