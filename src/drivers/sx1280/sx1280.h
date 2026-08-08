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
//                 On this board it is chip DIO2; any DIO works.
//
// EXTI constraints (STM32; see stm32h7_gpio.c for the dispatch)
//
//   Each interrupt pin claims the EXTI line of its pin NUMBER, and
//   same-numbered pins of all ports share one line: PA15 and PD15
//   are both line 15, so busy/dio1/dio_txdone must use three
//   distinct pin numbers. Route this deliberately on a PCB.
//
//   All three are registered at IRQ_LEVEL_IO. EXTI lines 5-9 and
//   10-15 are dispatched in two shared NVIC groups (one OS handler
//   fans out to the per-line callbacks), and mios requires every
//   line within a group to use the same IRQ level - so claiming a
//   grouped line here pins that whole group to IRQ_LEVEL_IO for any
//   other driver on the system.
sx1280_t *sx1280_create(spi_t *bus, gpio_t nss, gpio_t nreset,
                        gpio_t busy, gpio_t dio1, gpio_t dio_txdone,
                        const char *name);

error_t sx1280_reset(sx1280_t *s);

error_t sx1280_read_reg(sx1280_t *s, uint16_t addr, void *ptr, size_t len);

error_t sx1280_write_reg(sx1280_t *s, uint16_t addr, const void *ptr,
                         size_t len);
