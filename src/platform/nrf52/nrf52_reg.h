#pragma once

#include <stdint.h>

static inline void
reg_wr(uint32_t addr, uint32_t value)
{
  volatile uint32_t *ptr = (uint32_t *)addr;
  *ptr = value;
}

static inline uint32_t
reg_rd(uint32_t addr)
{
  volatile uint32_t *ptr = (uint32_t *)addr;
  return *ptr;
}

static inline void
reg_set(uint32_t addr, uint32_t mask)
{
  volatile uint32_t *ptr = (uint32_t *)addr;
  *ptr |= mask;
}

static inline int
reg_get_bit(uint32_t addr, int bit)
{
  return (reg_rd(addr) >> bit) & 1;
}


static inline void
reg_set_bits(uint32_t addr, uint32_t shift, uint32_t length, uint32_t bits)
{
  volatile uint32_t *ptr = (uint32_t *)addr;

  const uint32_t mask = ((1 << length) - 1) << shift;

  *ptr = (*ptr & ~mask) | ((bits << shift) & mask);
}


static inline void
reg_or(uint32_t addr, uint32_t bits)
{
  reg_wr(addr, reg_rd(addr) | bits);
}
