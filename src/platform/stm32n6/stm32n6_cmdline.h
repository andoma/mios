#pragma once

// Fixed address in the top page of the AXISRAM1+2 heap where the FSBL
// deposits the boot cmdline blob on serial boot. mios cmdline_init() (in
// stm32n6.c) reads it and copies the string out before this AXISRAM range is
// handed to the heap, so the page is then reused as normal heap (same
// read-then-reuse pattern as stm32h7).
//
// DTCM cannot be used: stm32n6_entry.S fills all of DTCM with a poison value
// on every boot to initialise its ECC syndrome bits, which would wipe the
// blob before cmdline_init() runs. AXISRAM is not poisoned.
//
// This address is above the FSBL stack (0x341c0000, grows down), above the
// downloaded image, and is not an ELF load target, so it survives the
// FSBL->mios handoff. On a cold boot it holds whatever; cmdline_init()'s
// magic/CRC checks then fail harmlessly. A blob is 8 + STM32N6_CMDLINE_SIZE
// + 4 = 204 bytes; stm32n6.c static-asserts it fits below the heap top.
#define STM32N6_CMDLINE_ADDR 0x241eef00u
#define STM32N6_CMDLINE_SIZE 192
