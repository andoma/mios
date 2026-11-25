#pragma once

/*
 * TPS92682-Q1 dual-channel LED controller, behind a vspi.c bus.
 *
 * Modelled from the datasheet (TI SLUSCX8C, sections 7.5 SPI and 7.6
 * registers), not from drivers/tps92682.c, so a suite running the driver
 * against it checks the driver against the part:
 *
 *  - 16-bit frames, CMD | A5..A0 | PAR | D7..D0, odd parity over the
 *    whole frame. A frame takes effect when chip select goes high.
 *  - The answer to a command comes in the next frame: a read response
 *    (SPE, '1100', RTO, PC, TW, data), a write response (the previous
 *    write echoed), or the Write Error/POR frame 0x8000 (the first frame
 *    after power-up, and after a write with a SPI error). More than 16
 *    clocks shift the received bits back out (daisy chain).
 *  - SPI error (SPE in the next response, the command not executed, a
 *    FLT read not clearing): a frame that is not a multiple of 16 bits,
 *    bad parity, or a read with non-zero data bits.
 *  - FLT1/FLT2 bits are set while their fault exists; a read clears the
 *    ones whose fault is gone, at the end of the read response frame.
 *  - PC is set at power-up and cleared by reading FLT1; while it is set,
 *    writes of 1 to CHxEN are blocked. RESET (0xc3 to 0x26) restores the
 *    writable registers and leaves PC alone.
 *  - A fault enabled in FEN1/FEN2 turns the channel off while it lasts;
 *    one latched in FLATEN keeps it off until EN is written with the
 *    channel's CHxEN bit set.
 *
 * Not modelled: limp home, soft start and fault timers (a non-latched
 * fault restarts the channel as soon as it is gone), daisy-chain decode,
 * the analog side. The test sets fault conditions (vtps92682_set_fault()),
 * the model does not derive them from currents and voltages.
 *
 * Assumed where the datasheet does not say: a fault disabled in FEN1/FEN2
 * does not set its FLT bit; a fault is only detected while its channel
 * is enabled (ISO and the shared TW/RTO always).
 */

#include <stdint.h>
#include <mios/io.h>

struct vtps92682;

// Per-channel fault conditions
#define VTPS92682_FAULT_UV    0x01  // Output undervoltage
#define VTPS92682_FAULT_OV    0x02  // Output overvoltage
#define VTPS92682_FAULT_ILIM  0x04  // Switch current limit
#define VTPS92682_FAULT_OC    0x08  // LED overcurrent
#define VTPS92682_FAULT_UC    0x10  // LED undercurrent (diagnostic)
#define VTPS92682_FAULT_ISO   0x20  // ISN pin open

// Shared conditions (channel argument ignored)
#define VTPS92682_FAULT_TW    0x40  // Thermal warning
#define VTPS92682_FAULT_RTO   0x80  // RT pin open

// Creates the chip powered up, attached to bus at cs
struct vtps92682 *vtps92682_create(spi_t *bus, gpio_t cs);

// Power-on reset: every register to its default, PC set, the next frame
// out is 0x8000. Fault conditions are left as they are.
void vtps92682_power_cycle(struct vtps92682 *t);

// Set or clear fault conditions on a channel (0 or 1)
void vtps92682_set_fault(struct vtps92682 *t, unsigned int ch,
                         uint8_t faults, int on);

// A chip that does not answer: MISO is not driven, frames have no effect
void vtps92682_set_dead(struct vtps92682 *t, int dead);

typedef struct vtps92682_output {
  int on;         // The channel is enabled and no fault holds it off
  uint8_t iadj;   // CHxIADJ, the analog current setting
  uint16_t pwm;   // Internal PWM duty, 0..1023, or 1023 with external PWM
} vtps92682_output_t;

vtps92682_output_t vtps92682_output(struct vtps92682 *t, unsigned int ch);

// A register's value without the side effects of a SPI read
// (FLT1/FLT2 are not cleared)
uint8_t vtps92682_peek(struct vtps92682 *t, uint8_t reg);

// Frames that had a SPI error
unsigned int vtps92682_spi_errors(struct vtps92682 *t);

// cb runs when a channel's output changes, with the bus locked: it must
// not call into the bus or the model
void vtps92682_set_listener(struct vtps92682 *t,
                            void (*cb)(void *opaque, struct vtps92682 *t,
                                       unsigned int ch),
                            void *opaque);
