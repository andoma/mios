#pragma once

#include <stdint.h>

#define FP_NAN       0
#define FP_INFINITE  1
#define FP_ZERO      2
#define FP_SUBNORMAL 3
#define FP_NORMAL    4

#define fpclassify(x) \
  __builtin_fpclassify(FP_NAN, FP_INFINITE, FP_ZERO, FP_SUBNORMAL, FP_NORMAL, x)

#define isnan(x) (fpclassify(x) == FP_NAN)
#define isfinite(x) (fpclassify(x) != FP_NAN && fpclassify(x) != FP_INFINITE)
#define isnormal(x) (fpclassify(x) == FP_NORMAL)
#define isinf(x) (fpclassify(x) == FP_INFINITE)

#define NAN __builtin_nanf("")
#define INFINITY __builtin_inff()

#define M_PIf 3.14159265358979f

#define M_HALF_PIf (0.5f * 3.14159265358979f)

#define M_2PIf (2.0f * 3.14159265358979f)

// ================
// Single precision
// ================

#if __ARM_FP & 4

static inline float __attribute__((always_inline))
fabsf(float f)
{
  float r;
  asm("vabs.f32 %0, %1" : "=t"(r) : "t" (f));
  return r;
}

static inline float __attribute__((always_inline))
sqrtf(float f)
{
  float r;
  asm("vsqrt.f32 %0, %1" : "=t"(r) : "t" (f));
  return r;
}

#else

static inline float
fabsf(float x)
{
  union {float f; uint32_t i;} u = {x};
  u.i &= 0x7fffffff;
  return u.f;
}

float sqrtf(float) __attribute__ ((const));

#endif


float sinf(float) __attribute__ ((const));

float cosf(float) __attribute__ ((const));

float tanf(float) __attribute__ ((const));

float frexprf(float x, int *e)  __attribute__ ((const));

float logf(float x)  __attribute__ ((const));

float expf(float x)  __attribute__ ((const));

float powf(float a, float b)  __attribute__ ((const));

float atanf(float)  __attribute__ ((const));

float atan2f(float, float)  __attribute__ ((const));

float asinf(float)  __attribute__ ((const));

float fmodf(float x, float y) __attribute__ ((const));


// ================
// Double precision
// ================

#if __ARM_FP & 8

static inline double __attribute__((always_inline))
sqrt(double f)
{
  double r;
  asm("vsqrt.f64 %P0, %P1" : "=w"(r) : "w" (f));
  return r;
}

#endif

// ================
// Half precision
// ================

uint16_t float32_to_float16(float f);

float float16_to_float32(uint16_t h);
