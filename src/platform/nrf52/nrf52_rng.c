#include <stdint.h>
#include <stdlib.h>

#include <mios/prng.h>

#include "nrf52_reg.h"
#include "nrf52_rng.h"
#include "nrf52_rtc.h"

static void  __attribute__((constructor(120)))
nrf52_rng_init(void)
{
  reg_wr(RNG_TASKS_START, 1);
}


int
rand(void)
{
  static prng_t state;
  uint32_t seed = (reg_rd(RNG_VALUE) << 24) | reg_rd(RTC2_BASE + RTC_COUNTER);
  return prng_get(&state, seed) & RAND_MAX;
}
