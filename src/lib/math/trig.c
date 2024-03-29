#include <stdint.h>

#include "math.h"

float
sinf(float x)
{
  const float spi = x < 0 ? -M_PIf : M_PIf;
  x = fmodf(x + spi, (2 * M_PIf)) - spi;

  if(x > M_HALF_PIf)
    x = -x + M_PIf;
  else if(x < -M_HALF_PIf)
    x = -x - M_PIf;

  const float x3 = x * x * x;
  const float x5 = x3 * x * x;
  const float x7 = x5 * x * x;

  return 0.99999660f * x - 0.16664824f * x3 + 0.00830629f * x5 - 0.00018363f * x7;
}


float
cosf(float x)
{
  return sinf(x + M_HALF_PIf);
}
