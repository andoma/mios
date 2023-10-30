#pragma once

#include <math.h>

static inline float
resistor_divider_get_top(float bottom_resistance, float voltage_ratio)
{
  return bottom_resistance * (voltage_ratio - 1.0f);
}

static inline float
resistor_divider_get_bottom(float top_resistance, float voltage_ratio)
{
  return top_resistance / (voltage_ratio - 1.0f);
}

static inline float
ntc_calc_temp(float beta, float ref_resistance,
              float ref_temp, float resistance)
{
  return (beta * ref_temp) / (beta + (ref_temp * logf(resistance / ref_resistance)));
}
