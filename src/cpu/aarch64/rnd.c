#include <stdint.h>
#include <stdlib.h>

#include <mios/prng.h>


int  __attribute__((weak))
rand(void)
{
  uint64_t src;
  asm volatile ("mrs %0, cntvct_el0\n\r" : "=r"(src));

  static prng_t state;

#ifdef TASK_ACCOUNTING
  src ^= cpu_cycle_counter();
#endif

  return prng_get(&state, src) & RAND_MAX;
}
