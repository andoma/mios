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
};
