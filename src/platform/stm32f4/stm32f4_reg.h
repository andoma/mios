#pragma once

#include <stdint.h>

static inline uint32_t
pr_bb(uint32_t addr, int bit)
{
  return ((bit * 4) + (addr << 5)) | 0x42000000;
}

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

static inline void
reg_set_bit(uint32_t addr, int bit)
{
  volatile uint32_t *ptr = (uint32_t *)pr_bb(addr, bit);
  *ptr = 1;
}

static inline void
reg_clr_bit(uint32_t addr, int bit)
{
  volatile uint32_t *ptr = (uint32_t *)pr_bb(addr, bit);
  *ptr = 0;
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

