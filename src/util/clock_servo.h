#pragma once

#include <mios/clock_realtime.h>

#define CLOCK_SERVO_MEDIAN_WINDOW 8

typedef struct clock_servo {
  clock_realtime_t *cs_clock;
  int64_t cs_accumulated_drift_ppb;
  int32_t cs_history[CLOCK_SERVO_MEDIAN_WINDOW];  // insertion order
  int32_t cs_sorted[CLOCK_SERVO_MEDIAN_WINDOW];   // always sorted
  uint8_t cs_sample_count;
  uint8_t cs_sample_write;
  uint8_t cs_synchronized;
} clock_servo_t;

void clock_servo_init(clock_servo_t *cs, clock_realtime_t *clk);

void clock_servo_adjust(clock_servo_t *cs, int64_t offset_ns,
                        int update_interval);

struct stream;
void clock_servo_print(const clock_servo_t *cs, struct stream *st);
