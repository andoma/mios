#pragma once

#include <stdint.h>

static inline void
reg_wr64(uint64_t addr, uint64_t value)
{
  volatile uint64_t *ptr = (uint64_t *)addr;
  *ptr = value;
}

static inline void
reg_wr(uint64_t addr, uint32_t value)
{
  volatile uint32_t *ptr = (uint32_t *)addr;
  *ptr = value;
}

static inline void
reg_wr8(uint64_t addr, uint8_t value)
{
  volatile uint8_t *ptr = (uint8_t *)addr;
  *ptr = value;
}

static inline void
reg_wr16(uint64_t addr, uint16_t value)
{
  volatile uint16_t *ptr = (uint16_t *)addr;
  *ptr = value;
}

static inline uint64_t
reg_rd64(uint64_t addr)
{
  volatile uint64_t *ptr = (uint64_t *)addr;
  return *ptr;
}

static inline uint32_t
reg_rd(uint64_t addr)
{
  volatile uint32_t *ptr = (uint32_t *)addr;
  return *ptr;
}

static inline uint16_t
reg_rd16(uint64_t addr)
{
  volatile uint16_t *ptr = (uint16_t *)addr;
  return *ptr;
}

static inline uint8_t
reg_rd8(uint64_t addr)
{
  volatile uint8_t *ptr = (uint8_t *)addr;
  return *ptr;
}


static inline void
reg_set_bits(uint64_t addr, uint32_t shift, uint32_t length, uint32_t bits)
{
  volatile uint32_t *ptr = (uint32_t *)addr;

  const uint32_t mask = ((1 << length) - 1) << shift;

  *ptr = (*ptr & ~mask) | ((bits << shift) & mask);
}

static inline void
reg_set_bit(uint64_t addr, uint32_t bit)
{
  reg_set_bits(addr, bit, 1, 1);
}

static inline void
reg_clr_bit(uint64_t addr, uint32_t bit)
{
  reg_set_bits(addr, bit, 1, 0);
}
