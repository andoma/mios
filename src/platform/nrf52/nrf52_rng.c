#include <stdint.h>
#include <stdlib.h>

#include <mios/prng.h>

#include "nrf52_reg.h"
#include "nrf52_rng.h"
#include "nrf52_rtc.h"

static prng_t state;

static void  __attribute__((constructor(120)))
nrf52_rng_init(void)
{
  // Use hardware RNG for initial seed
  reg_wr(RNG_TASKS_START, 1);
  reg_wr(RNG_CONFIG, 1); // Enable BIAS correction

  for(int i = 0; i < 8; i++) {
    while(reg_rd(RNG_EVENTS_VALRDY) == 0) {
    }
    prng_get(&state, reg_rd(RNG_VALUE));
  }
  reg_wr(RNG_TASKS_STOP, 1);
}


int
rand(void)
{
  uint32_t seed = reg_rd(RTC2_BASE + RTC_COUNTER);
  return prng_get(&state, seed) & RAND_MAX;
}
