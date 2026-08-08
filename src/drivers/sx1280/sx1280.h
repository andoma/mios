#pragma once

#include <mios/io.h>

typedef struct sx1280 sx1280_t;

// Semtech SX1280 2.4GHz transceiver.
//
// Pin requirements
//
//   nss, nreset   Plain outputs, any pins.
//
//   busy          Input with a falling-edge interrupt. The command
//                 handshake sleeps on it.
//
//   dio1          Input with a rising-edge interrupt. Carries the
//                 radio IRQs (RxDone, TxDone, timeouts, ...).
//
//   dio_txdone    Optional (pass GPIO_UNUSED). A chip DIO carrying
//                 TX_DONE alone, so the AutoTx completion is observed
//                 as its own interrupt. Needed because ANY
//                 ClrIrqStatus during the AutoTx countdown cancels
//                 the pending transmission (undocumented; selective
//                 bit masks included), so DIO1 - which is left
//                 asserted by the just-received packet - cannot
//                 signal it. Without this pin the driver falls back
//                 to polling GetIrqStatus over SPI, which is slow
//                 enough to break multi-PDU BLE connection events.
//                 Any chip DIO works; pick one when routing.
//
// Choosing interrupt pins
//
//   All three interrupts are registered at IRQ_LEVEL_IO via
//   gpio_conf_irq(). What makes a set of pins valid is platform
//   specific - check the platform's gpio code before routing a PCB:
//
//   - On STM32 parts, same-numbered pins of all ports share one EXTI
//     line (PA15 and PD15 are both line 15), so the three pins must
//     use distinct pin numbers.
//
//   - Some parts also gang EXTI lines into shared CPU interrupts
//     (e.g. STM32H7: lines 5-9 and 10-15 form two groups, dispatched
//     by one OS handler each), and mios then requires every line in
//     a group to use the same IRQ level - claiming a grouped line
//     here pins that group to IRQ_LEVEL_IO for all other drivers.
//     Parts with per-line interrupts (e.g. STM32N6) have no such
//     coupling.
sx1280_t *sx1280_create(spi_t *bus, gpio_t nss, gpio_t nreset,
                        gpio_t busy, gpio_t dio1, gpio_t dio_txdone,
                        const char *name);

error_t sx1280_reset(sx1280_t *s);

error_t sx1280_read_reg(sx1280_t *s, uint16_t addr, void *ptr, size_t len);

error_t sx1280_write_reg(sx1280_t *s, uint16_t addr, const void *ptr,
                         size_t len);
