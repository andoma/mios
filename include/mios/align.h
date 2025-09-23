#pragma once

static inline unsigned int
round_down_pow2(unsigned int x)
{
    if (x == 0)
      return 0;

    return 1U << (31 - __builtin_clz(x));
}
