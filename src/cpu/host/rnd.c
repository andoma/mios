#include <stdint.h>
#include <stdlib.h>

#include <mios/prng.h>

#include "cpu.h"
#include "linux.h"

#define SYS_getrandom 318

int  __attribute__((weak))
rand(void)
{
  static prng_t state;
  static int seeded;

  uint32_t src = cpu_cycle_counter();
  if(!seeded) {
    uint32_t seed = 0;
    linux_syscall(SYS_getrandom, &seed, sizeof(seed), 0);
    src ^= seed;
    seeded = 1;
  }
  return prng_get(&state, src) & RAND_MAX;
}
