#include <math.h>

float fmodf(float x, float y)
{
  return x - (int)(x / y) * y;
}
