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
