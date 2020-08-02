#include <stdint.h>

#include "math.h"

float
MATH_MANGLE(sinf)(float x)
{
  if(x > M_HALF_PIf)
    x = -x + M_PIf;
  else if(x < -M_HALF_PIf)
    x = -x - M_PIf;

  const float x3 = x * x * x;
  const float x5 = x3 * x * x;
  const float x7 = x5 * x * x;

  return 0.99999660f * x - 0.16664824f * x3 + 0.00830629f * x5 - 0.00018363f * \
x7;
}


float
MATH_MANGLE(cosf)(float x)
{
  return MATH_MANGLE(sinf)(x + M_HALF_PIf);
}

float
MATH_MANGLE(powf)(float a, float b)
{
  return __builtin_powf(a, b);
}
