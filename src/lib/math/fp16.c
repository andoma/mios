#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>

uint16_t
float32_to_float16(float f)
{
  union { float f; uint32_t u; } x = { f };
  uint32_t s = (x.u >> 16) & 0x8000;
  uint32_t e = (x.u >> 23) & 0xFF;
  uint32_t m = x.u & 0x7FFFFF;

  if (e == 255) return s | (m ? 0x7E00 : 0x7C00); // NaN or Inf

  int new_exp = (int)e - 127 + 15;
  if (new_exp >= 31) return s | 0x7C00;           // overflow to Inf
  if (new_exp <= 0) return s;                     // underflow or denormal -> 0

  // Add rounding bit (round to nearest, ties to even)
  m += 0x1000; // 13th bit = rounding bit

  if (m & 0x800000) { // mantissa overflow after rounding
    m = 0;
    new_exp++;
    if (new_exp >= 31) return s | 0x7C00; // overflow after rounding
  }

  uint16_t he = (uint16_t)(new_exp << 10);
  uint16_t hm = (uint16_t)(m >> 13);
  return s | he | hm;
}


float
float16_to_float32(uint16_t h)
{
  uint32_t s = (h & 0x8000) << 16;
  uint32_t e = (h >> 10) & 0x1F;
  uint32_t m = h & 0x3FF;

  uint32_t out;
  if (e == 0) {
    if (m == 0) {
      out = s;  // signed zero
    } else {
      // Denormal: normalize it
      e = 1;
      while ((m & 0x400) == 0) {
        m <<= 1;
        e--;
      }
      m &= 0x3FF; // remove leading 1
      e += 127 - 15;
      out = s | (e << 23) | (m << 13);
    }
  } else if (e == 31) {
    out = s | 0x7F800000 | (m << 13);  // Inf or NaN
  } else {
    e = e + 127 - 15;
    out = s | (e << 23) | (m << 13);   // Normalized
  }

  union { uint32_t u; float f; } result = { out };
  return result.f;
}
