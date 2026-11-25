#pragma once

/*
 * TCAL9539-Q1 16-bit I2C I/O expander, behind a vi2c.c bus.
 *
 * Modelled from the datasheet (TI SCPS285A, section 8.6), not from
 * drivers/tcal9539.c:
 *
 *  - A write starts with the command byte, which points at a register;
 *    every data byte after it (written or read, also after a repeated
 *    START) goes to the other register of the pair (input, output,
 *    polarity, configuration, drive strength x2, input latch, pull
 *    enable, pull select, interrupt mask, interrupt status).
 *  - Input registers read the level on the pins whatever their
 *    direction, inverted where the polarity register says so for
 *    inputs. Output registers read the flip-flop, not the pin.
 *  - A pin's level: what it drives as an output (port-wise push-pull or
 *    open drain, register 4F), else what is driven on it from outside
 *    (vtcal9539_drive()), else its pull if enabled, else 0.
 *  - Interrupt: an input whose level differs from the one the last read
 *    of its input register returned. A read of the input register clears
 *    it, as does the input returning to that level or the pin being made
 *    an output. Unmasked, it pulls INT low (open drain, vgpio.c pin).
 *    The interrupt status registers show the unmasked ones.
 *  - Power-on and RESET put every register at its default: all inputs,
 *    outputs at 1, no pulls, all interrupts masked.
 *
 * Not modelled: the input latch (the register is stored, inputs are
 * never latched), drive strength (stored), the software reset general
 * call. Assumed: a command byte naming no register is ACKed, its reads
 * return 0 and writes are ignored.
 */

#include <stdint.h>
#include <mios/io.h>

struct vtcal9539;

// int_pin is the vgpio.c pin INT is wired to, or GPIO_UNUSED
struct vtcal9539 *vtcal9539_create(i2c_t *bus, uint8_t addr, gpio_t int_pin);

// Drive line (0..15) from outside with level, or stop driving it
void vtcal9539_drive(struct vtcal9539 *t, unsigned int line, int level);
void vtcal9539_release(struct vtcal9539 *t, unsigned int line);

// The level on line
int vtcal9539_pin(struct vtcal9539 *t, unsigned int line);

// RESET_N pulsed, or a power cycle: every register back to its default
void vtcal9539_reset(struct vtcal9539 *t);
