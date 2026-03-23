/**
 * @file pwg.h
 * @brief Pulse Waveform Generator (PWG)
 *
 * The PWG generates jitter-free pulse trains by using a hardware timer
 * to trigger DMA transfers from a memory buffer to a GPIO port's BSRR
 * (Bit Set/Reset Register). This produces cycle-accurate pin toggling
 * with zero CPU involvement during steady-state operation.
 *
 * A background thread refills the inactive half of a circular
 * double-buffer via a user-supplied callback. At the configured
 * timer frequency, DMA writes one 32-bit word per tick to BSRR:
 *
 *   Bits  0-15: Set corresponding GPIO pin high
 *   Bits 16-31: Reset corresponding GPIO pin low (bit 16 = pin 0, etc.)
 *
 * Writing 0 to a BSRR word leaves all pins unchanged for that tick.
 *
 * All controlled pins must reside on the same GPIO port. For pins
 * on different ports, create separate PWG instances.
 *
 * Typical use: stepper motor control where STEP and DIR pins on the
 * same port need precisely timed pulse sequences.
 */

#pragma once

#include <stdint.h>
#include <stddef.h>

/**
 * Callback invoked by the PWG fill thread whenever a half-buffer
 * needs new data.
 *
 * @param opaque   User-supplied context pointer
 * @param buf      Pointer to the half-buffer to fill (32 entries)
 * @param count    Number of uint32_t entries to write
 *
 * Each entry is a BSRR value that will be written to the GPIO port
 * on successive timer ticks. The callback must fill all 'count'
 * entries before returning.
 */
typedef void (*pwg_fill_cb)(void *opaque, uint32_t *buf, size_t count);
