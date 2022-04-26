#pragma once

#include <mios/mios.h>

#define HZ 100
#define TICKS_PER_US (CPU_SYSTICK_RVR / 1000000)
#define TICKS_PER_HZ (CPU_SYSTICK_RVR / HZ)

static inline __attribute__((always_inline)) int clock_unwrap(void)
{
  uint64_t clock;
  static volatile unsigned int * const SYST_CSR = (unsigned int *)0xe000e010;
  if(unlikely(*SYST_CSR & 0x10000)) {
    clock += 1000000 / HZ;
    return 1;
  }
  return 0;
}
