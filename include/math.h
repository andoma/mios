#pragma once

#ifdef MATH_PREFIX

#define MATH_MANGLE(x) mios_##x

#else

#define MATH_MANGLE(x) x

#endif

#define M_PIf 3.14159265358979f

#define M_HALF_PIf (0.5f * 3.14159265358979f)

#ifdef __ARM_FP

static inline float
MATH_MANGLE(fabsf)(float f)
{
  float r;
  asm("vabs.f32 %0, %1" : "=t"(r) : "t" (f));
  return r;
}

static inline float
MATH_MANGLE(sqrtf)(float f)
{
  float r;
  asm("vsqrt.f32 %0, %1" : "=t"(r) : "t" (f));
  return r;
}

static inline float
MATH_MANGLE(fmaf)(float x, float y, float z)
{
  float r;
  asm("vfma.f32 %0, %1, %2, %3" : "=t"(r) : "t" (x), "t" (y), "t" (z));
  return r;
}

#endif

float MATH_MANGLE(sinf)(float) __attribute__ ((const));

float MATH_MANGLE(cosf)(float) __attribute__ ((const));

float MATH_MANGLE(powf)(float a, float b)  __attribute__ ((const));
