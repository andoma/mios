#include "clock_servo.h"

#include <mios/eventlog.h>
#include <stdio.h>
#include <string.h>
#include <inttypes.h>

#define STEP_THRESHOLD_NS  100000000LL  // 100 ms
#define MAX_ADJ_PPB        100000
#define SERVO_KP_SHIFT     2
#define SERVO_KI_SHIFT     7


void
clock_servo_init(clock_servo_t *cs, clock_realtime_t *clk)
{
  cs->cs_clock = clk;
  cs->cs_accumulated_drift_ppb = 0;
  cs->cs_sample_count = 0;
  cs->cs_sample_write = 0;
}


static int32_t
median_filter(clock_servo_t *cs, int32_t offset_ns)
{
  // Remove oldest sample from sorted array if window is full
  if(cs->cs_sample_count == CLOCK_SERVO_MEDIAN_WINDOW) {
    int32_t old = cs->cs_history[cs->cs_sample_write];
    int i;
    for(i = 0; i < cs->cs_sample_count; i++) {
      if(cs->cs_sorted[i] == old)
        break;
    }
    memmove(&cs->cs_sorted[i], &cs->cs_sorted[i + 1],
            (cs->cs_sample_count - i - 1) * sizeof(int32_t));
    cs->cs_sample_count--;
  }

  // Insert new sample into sorted array
  int i;
  for(i = cs->cs_sample_count; i > 0 && cs->cs_sorted[i - 1] > offset_ns; i--)
    cs->cs_sorted[i] = cs->cs_sorted[i - 1];
  cs->cs_sorted[i] = offset_ns;
  cs->cs_sample_count++;

  // Store in history ring
  cs->cs_history[cs->cs_sample_write] = offset_ns;
  cs->cs_sample_write =
    (cs->cs_sample_write + 1) % CLOCK_SERVO_MEDIAN_WINDOW;

  return cs->cs_sorted[cs->cs_sample_count / 2];
}


static void
clock_servo_step(clock_servo_t *cs, int64_t offset_ns)
{
  clock_realtime_t *clk = cs->cs_clock;

  int64_t cur_ns = clk->clk_class->get_time(clk);
  clk->clk_class->set_time(clk, cur_ns + offset_ns);

  cs->cs_accumulated_drift_ppb = 0;
  cs->cs_sample_count = 0;
  cs->cs_sample_write = 0;

  evlog(LOG_DEBUG, "%s: Step adjust %" PRId64 " ns", clk->clk_class->name, offset_ns);
}


static void
clock_servo_slew(clock_servo_t *cs, int64_t offset_ns, int update_interval)
{
  int64_t filtered = median_filter(cs, offset_ns);

  int64_t adj_p = filtered >> SERVO_KP_SHIFT;
  cs->cs_accumulated_drift_ppb +=
    (filtered >> SERVO_KI_SHIFT) * update_interval;

  // Anti-windup clamping
  if(cs->cs_accumulated_drift_ppb > MAX_ADJ_PPB)
    cs->cs_accumulated_drift_ppb = MAX_ADJ_PPB;
  else if(cs->cs_accumulated_drift_ppb < -MAX_ADJ_PPB)
    cs->cs_accumulated_drift_ppb = -MAX_ADJ_PPB;

  int64_t total_ppb = adj_p + cs->cs_accumulated_drift_ppb;

  clock_realtime_t *clk = cs->cs_clock;
  clk->clk_class->adj_time(clk, (int32_t)total_ppb);
}


void
clock_servo_adjust(clock_servo_t *cs, int64_t offset_ns, int update_interval)
{
  int64_t abs_offset = (offset_ns < 0) ? -offset_ns : offset_ns;

  if(abs_offset > STEP_THRESHOLD_NS) {
    clock_servo_step(cs, offset_ns);
  } else {
    clock_servo_slew(cs, offset_ns, update_interval);
  }
  cs->cs_clock->synchronized = 1;
}


static int32_t
isqrt64(int64_t x)
{
  if(x <= 0)
    return 0;
  int64_t r = 0;
  int64_t bit = (int64_t)1 << 30;
  while(bit > x)
    bit >>= 2;
  while(bit) {
    if(x >= r + bit) {
      x -= r + bit;
      r = (r >> 1) + bit;
    } else {
      r >>= 1;
    }
    bit >>= 2;
  }
  return (int32_t)r;
}


void
clock_servo_print(const clock_servo_t *cs, struct stream *st)
{
  stprintf(st, "  Freq drift: %"PRId64" ppb\n",
           cs->cs_accumulated_drift_ppb);

  int n = cs->cs_sample_count;
  if(n < 2) {
    stprintf(st, "  Jitter: insufficient samples\n");
    return;
  }

  // Compute mean
  int64_t sum = 0;
  for(int i = 0; i < n; i++)
    sum += cs->cs_sorted[i];
  int32_t mean = sum / n;

  // Compute variance (integer)
  int64_t var_sum = 0;
  for(int i = 0; i < n; i++) {
    int64_t d = cs->cs_sorted[i] - mean;
    var_sum += d * d;
  }
  int32_t stddev = isqrt64(var_sum / n);

  stprintf(st, "  Jitter: stddev %d ns  min %d ns  max %d ns\n",
           stddev, cs->cs_sorted[0], cs->cs_sorted[n - 1]);
}
