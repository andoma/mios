#pragma once


// Only works for fifo size that is:
//   Power of two
//   AND less than 256 bytes

#define FIFO_DECL(x, siz)                       \
  struct {                                      \
    uint8_t rdptr;                              \
    uint8_t wrptr;                              \
    uint8_t buf[siz];                           \
  } x;

#define fifo_is_empty(x) ((x)->wrptr == (x)->rdptr)

#define fifo_used(x) ((uint8_t)((x)->wrptr - (x)->rdptr))

#define fifo_avail(x) (sizeof((x)->buf) - fifo_used(x))

#define fifo_is_full(x) (fifo_avail(x) == 0)

#define fifo_wr(x, y) (x)->buf[(x)->wrptr++ & (sizeof((x)->buf) - 1)] = y

#define fifo_rd(x) (x)->buf[(x)->rdptr++ & (sizeof((x)->buf) - 1)]

