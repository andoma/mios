#include <stddef.h>
#include <stdint.h>

#include "nrf52_reg.h"
#include "nrf52_rng.h"
#include "nrf_sdc.h"

// The nRF52 RNG peripheral: a hardware noise source feeding one byte at a
// time. Used for the SDC rand source and SMP pairing nonces. MPSL does not
// touch RNG, so we own it outright once BLE is up.

void
nrf_trng_init(void)
{
  reg_wr(RNG_CONFIG, 1);     // DERCEN: bias correction on the raw noise
  reg_wr(RNG_EVENTS_VALRDY, 0);
  reg_wr(RNG_TASKS_START, 1);
}

void
nrf_trng_read(uint8_t *buf, size_t len)
{
  while(len) {
    while(reg_rd(RNG_EVENTS_VALRDY) == 0) {}
    reg_wr(RNG_EVENTS_VALRDY, 0);
    *buf++ = reg_rd(RNG_VALUE);
    len--;
  }
}
