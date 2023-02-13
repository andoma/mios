#include <stdint.h>
#include <stdlib.h>

#include <mios/prng.h>

#include "nrf52_reg.h"
#include "nrf52_rng.h"

static void  __attribute__((constructor(120)))
nrf52_rng_init(void)
{
  reg_wr(RNG_TASKS_START, 1);
}


int
rand(void)
{
  static volatile unsigned int * const SYST_VAL = (unsigned int *)0xe000e018;
  static prng_t state;
  return prng_get(&state, reg_rd(RNG_VALUE) ^ *SYST_VAL) & RAND_MAX;
}
