#pragma once

#ifdef __ARM_FP


static inline float
fabsf(float f)
{
  float r;
  asm volatile("vabs.f32 %0, %1" : "=t"(r) : "t" (f));
  return r;
}

static inline float
sqrtf(float f)
{
  float r;
  asm volatile("vsqrt.f32 %0, %1" : "=t"(r) : "t" (f));
  return r;
}

#endif
