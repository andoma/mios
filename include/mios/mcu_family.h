#pragma once

// Coarse chip-family tag, compiled into every image (see version.c) so
// host-side flash tooling can refuse to write an image to the wrong
// physical chip (e.g. an md1/STM32G4 image onto the fc1/STM32H7 board).
// Values must stay stable once shipped; they're compared against live
// hardware identification, not just other builds.

#define MCU_FAMILY_UNKNOWN   0
#define MCU_FAMILY_STM32H7   1
#define MCU_FAMILY_STM32G4   2
