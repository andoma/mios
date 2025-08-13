#pragma once

#include "type_macros.h"

// Only works for fifo size that is:
//   Power of two
//   AND less than 256 bytes

#define FIFO_DECL(x, type, siz)                 \
  struct {                                      \
    uint8_t rdptr;                              \
    uint8_t wrptr;                              \
    type buf[siz];                              \
  } x;

#define fifo_is_empty(x) ((x)->wrptr == (x)->rdptr)

#define fifo_used(x) ((uint8_t)((x)->wrptr - (x)->rdptr))

#define fifo_avail(x) (ARRAYSIZE((x)->buf) - fifo_used(x))

#define fifo_is_full(x) (fifo_avail(x) == 0)

#define fifo_wr(x, y) (x)->buf[(x)->wrptr++ & (ARRAYSIZE((x)->buf) - 1)] = y

#define fifo_rd(x) (x)->buf[(x)->rdptr++ & (ARRAYSIZE((x)->buf) - 1)]

