#pragma once

#include <mios/mios.h>

#define HZ 100

static inline __attribute__((always_inline)) int clock_unwrap(void)
{
  extern uint64_t clock;
  static volatile unsigned int * const SYST_CSR = (unsigned int *)0xe000e010;
  if(unlikely(*SYST_CSR & 0x10000)) {
    clock += 1000000 / HZ;
    return 1;
  }
  return 0;
}
