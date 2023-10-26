/*
 * Adapted for MIOS by Andreas Smas 2023
 */

/* Copyright (C) 2011  Julien Pommier
 *
 *  This software is provided 'as-is', without any express or implied
 *  warranty.  In no event will the authors be held liable for any damages
 *  arising from the use of this software.
 *
 *  Permission is granted to anyone to use this software for any purpose,
 *  including commercial applications, and to alter it and redistribute it
 *  freely, subject to the following restrictions:
 *
 *  1. The origin of this software must not be misrepresented; you must not
 *     claim that you wrote the original software. If you use this software
 *     in a product, an acknowledgment in the product documentation would be
 *     appreciated but is not required.
 *  2. Altered source versions must be plainly marked as such, and must not be
 *     misrepresented as being the original software.
 *  3. This notice may not be removed or altered from any source distribution.
 *
 *  (this is the zlib license)
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <sys/param.h>

#define SQRTHF 0.707106781186547524

#define log_p0 7.0376836292E-2
#define log_p1 -1.1514610310E-1
#define log_p2 1.1676998740E-1
#define log_p3 -1.2420140846E-1
#define log_p4 +1.4249322787E-1
#define log_p5 -1.6668057665E-1
#define log_p6 +2.0000714765E-1
#define log_p7 -2.4999993993E-1
#define log_p8 +3.3333331174E-1
#define log_q1 -2.12194440e-4
#define log_q2 0.693359375

static uint32_t
flt_as_u32(float f)
{
  uint32_t u32;
  memcpy(&u32, &f, sizeof(uint32_t));
  return u32;
}

static float
u32_as_flt(uint32_t u32)
{
  float f;
  memcpy(&f, &u32, sizeof(uint32_t));
  return f;
}

float
frexpf(float x, int *e)
{
  uint32_t u32 = flt_as_u32(x);
  *e = (u32 >> 23) - 126;
  u32 &= 0x807fffff;
  u32 |= 0x3f000000;
  return u32_as_flt(u32);
}

float
logf(float x)
{
  if(x < 0)
    return NAN;

  int emm0;
  x = frexpf(x, &emm0);
  float e = emm0;

  if(x < (float)SQRTHF) {
    e--;
    x = x + x - 1.0f;
  } else {
    x = x - 1.0f;
  }

  float z = x * x;
  float y = (float)log_p0;
  y = (float)log_p1 + y * x;
  y = (float)log_p2 + y * x;
  y = (float)log_p3 + y * x;
  y = (float)log_p4 + y * x;
  y = (float)log_p5 + y * x;
  y = (float)log_p6 + y * x;
  y = (float)log_p7 + y * x;
  y = (float)log_p8 + y * x;
  y = y * x;
  y = y * z;

  y = y + e * (float)log_q1;
  y = y - z * 0.5f;
  x = x + y;
  x = x + e * (float)log_q2;
  return x;
}


#define c_exp_hi 88.3762626647949f
#define c_exp_lo -88.3762626647949f
#define LOG2EF 1.44269504088896341
#define exp_C1 0.693359375
#define exp_C2 -2.12194440e-4
#define exp_p0 1.9875691500E-4
#define exp_p1 1.3981999507E-3
#define exp_p2 8.3334519073E-3
#define exp_p3 4.1665795894E-2
#define exp_p4 1.6666665459E-1
#define exp_p5 5.0000001201E-1

float
expf(float x)
{
  if(x > (float)c_exp_hi)
    return INFINITY;
  if(x < (float)c_exp_lo)
    return -INFINITY;

  float z = (float)LOG2EF * x + 0.5f;
  z = (int)z;

  int n = z;
  float pow2n = u32_as_flt((n + 127) << 23);

  x -= z * (float)exp_C1;
  x -= z * (float)exp_C2;

  z = x * x;

  float y = exp_p0;
  y = (float)exp_p1 + y * x;
  y = (float)exp_p2 + y * x;
  y = (float)exp_p3 + y * x;
  y = (float)exp_p4 + y * x;
  y = (float)exp_p5 + y * x;
  y = x + y * z;
  y = y + 1.0f;
  y = y * pow2n;
  return y;
}


float
powf(float a, float b)
{
  return expf(b * logf(a));
}
