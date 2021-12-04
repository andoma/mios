#pragma once

#include <stdint.h>

struct ntcpoly {
  int32_t K0, K1, K2, K3;
  uint8_t s0, s1, s2, s3, r;
};

int ntcpoly_compute(int32_t x, const struct ntcpoly *np);

