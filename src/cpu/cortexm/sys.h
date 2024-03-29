#pragma once

inline void  __attribute__((always_inline))
schedule(void)
{
  volatile unsigned int * const ICSR = (unsigned int *)0xe000ed04;
  *ICSR = 1 << 28; // Raise PendSV interrupt
  asm volatile("" ::: "memory");
}
