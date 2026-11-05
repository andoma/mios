#pragma once

#include <stdint.h>

typedef struct clock_realtime clock_realtime_t;

typedef struct clock_realtime_class {
  const char *name;
  void (*set_time)(clock_realtime_t *clk, int64_t nsec);
  int64_t (*get_time)(clock_realtime_t *clk);
  void (*adj_time)(clock_realtime_t *clk, int32_t ppb);
} clock_realtime_class_t;

struct clock_realtime {
  const clock_realtime_class_t *clk_class;
  uint8_t synchronized;   // set by clock_servo when first adjusted
};

// Returns 0 if the clock has not yet been synchronized to an external
// time source (e.g. PTP master), otherwise the underlying class's
// timestamp in nanoseconds.
static inline int64_t
clock_realtime_get_time(clock_realtime_t *clk)
{
  if(!clk->synchronized)
    return 0;
  return clk->clk_class->get_time(clk);
}

// The addend of a timestamp unit in fine correction mode (the STM32
// Ethernet MACs) for a frequency correction of `ppb` on top of the
// `nominal` one, saturated to the 32-bit register rather than wrapped.
// The nominal addend can sit close to 2^32 (0xfdf7df7d on an STM32N6
// counting from a 48 MHz HSE, 0.8 % below it) while the servo asks for
// several percent to slew out a large offset; wrapped, the addend all
// but stops the clock.
static inline uint32_t
clock_realtime_addend(uint32_t nominal, int32_t ppb)
{
  const int64_t a = nominal + (int64_t)nominal * ppb / 1000000000;
  if(a > UINT32_MAX)
    return UINT32_MAX;
  if(a < 0)
    return 0;
  return a;
}
