#include <stdint.h>
#include <stdlib.h>

#include <mios/prng.h>

#include "cpu.h"
#include "linux.h"

#define SYS_getrandom 318

static prng_t state;
static int seeded;
static uint32_t seq;

// Deterministic sequence for test suites. Same seed, same numbers.
void
host_rand_seed(uint32_t seed)
{
  state.a = 0xf1ea5eed;
  state.b = state.c = state.d = seed;
  for(int i = 0; i < 20; i++)
    prng_get(&state, 0);
  seq = 0;
  seeded = 2;
}

int  __attribute__((weak))
rand(void)
{
  uint32_t src;

  if(seeded == 2) {
    src = seq++;
  } else {
    src = cpu_cycle_counter();
    if(!seeded) {
      uint32_t seed = 0;
      linux_syscall(SYS_getrandom, &seed, sizeof(seed), 0);
      src ^= seed;
      seeded = 1;
    }
  }
  return prng_get(&state, src) & RAND_MAX;
}
