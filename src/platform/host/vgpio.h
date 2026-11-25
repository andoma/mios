#pragma once

/*
 * GPIO pins of the host process.
 *
 * Implements the io.h GPIO API (gpio_conf_output(), gpio_set_output(),
 * gpio_get_input(), gpio_conf_irq(), ...) over an array of pin states,
 * so board code and drivers run unmodified, and adds the other side of
 * the pin for the things a board connects to it: device models (vspi.c
 * selects a chip by the level of its chip select pin, an expander model
 * pulls its INT line low) and test suites.
 *
 * A pin behaves like an STM32 pin:
 *
 *  - The output latch exists in every mode. gpio_set_output() on a pin
 *    that is still an input sets the latch, and the pin drives that value
 *    once it is made an output (board code relies on this to avoid a
 *    glitch on chip selects).
 *
 *  - gpio_get_input() returns the level on the pin in every mode,
 *    including the level an output drives.
 *
 *  - The level of a pin that does not drive (input, analog, alternate
 *    function, or an open-drain output at 1) is what something outside
 *    drives on it (vgpio_drive()), else its pull, else 0. A real floating
 *    input reads whatever; 0 keeps runs deterministic.
 *
 *  - An IRQ (gpio_conf_irq()) fires on the configured edge of that level,
 *    at its IRQ level, through a host IRQ line.
 *
 * Every pin starts as an input with no pull and the latch at 0.
 */

#include <mios/io.h>

// What the pin drives: 0 or 1, or -1 when it does not drive (not an
// output, or an open-drain output at 1)
int vgpio_driven(gpio_t gpio);

// The level on the pin, as gpio_get_input() reads it
int vgpio_level(gpio_t gpio);

// 1 if nothing sets the pin's level: it does not drive, nothing drives it
// from outside and it has no pull (vgpio_level() then reads 0)
int vgpio_floating(gpio_t gpio);

// Drive the pin from outside with level (0 or 1). An output pin that
// drives as well wins: the level is what the pin drives.
void vgpio_drive(gpio_t gpio, int level);

// Stop driving the pin from outside (its pull decides again)
void vgpio_release(gpio_t gpio);
