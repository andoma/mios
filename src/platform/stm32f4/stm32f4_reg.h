#pragma once

#include <stdint.h>

__attribute__((always_inline))
static inline void
reg_wr(uint32_t addr, uint32_t value)
{
  volatile uint32_t *ptr = (uint32_t *)addr;
  *ptr = value;
}

__attribute__((always_inline))
static inline void
reg_wr8(uint32_t addr, uint8_t value)
{
  volatile uint8_t *ptr = (uint8_t *)addr;
  *ptr = value;
}

__attribute__((always_inline))
static inline uint32_t
reg_rd(uint32_t addr)
{
  volatile uint32_t *ptr = (uint32_t *)addr;
  return *ptr;
}

__attribute__((always_inline))
static inline void
reg_set(uint32_t addr, uint32_t mask)
{
  volatile uint32_t *ptr = (uint32_t *)addr;
  *ptr |= mask;
}

__attribute__((always_inline))
static inline int
reg_get_bit(uint32_t addr, int bit)
{
  return (reg_rd(addr) >> bit) & 1;
}


__attribute__((always_inline))
static inline void
reg_set_bits(uint32_t addr, uint32_t shift, uint32_t length, uint32_t bits)
{
  volatile uint32_t *ptr = (uint32_t *)addr;

  const uint32_t mask = ((1 << length) - 1) << shift;

  *ptr = (*ptr & ~mask) | ((bits << shift) & mask);
}

__attribute__((always_inline))
static inline uint32_t
pr_bb(uint32_t addr, int bit)
{
  return ((bit * 4) + (addr << 5)) | 0x02000000 | (addr & 0xf0000000);
}

#define reg_set_bit(addr, bit) do {                             \
    if(__builtin_constant_p(addr) &&                            \
       (addr) >= 0x40000000 && (addr) < 0x41000000) {           \
      volatile uint32_t *ptr = (uint32_t *)pr_bb(addr, bit);    \
      *ptr = 1;                                                 \
    } else {                                                    \
      reg_set_bits(addr, bit, 1, 1);                            \
    }                                                           \
  } while(0)

#define reg_clr_bit(addr, bit) do {                             \
    if(__builtin_constant_p(addr) &&                            \
     (addr) >= 0x40000000 && (addr) < 0x41000000) {             \
    volatile uint32_t *ptr = (uint32_t *)pr_bb(addr, bit);      \
    *ptr = 0;                                                   \
  } else {                                                      \
    reg_set_bits(addr, bit, 1, 0);                              \
  }                                                             \
} while(0)

__attribute__((always_inline))
static inline void
reg_or(uint32_t addr, uint32_t bits)
{
  reg_wr(addr, reg_rd(addr) | bits);
}
