#pragma once

#include <stdint.h>

static inline void
reg_wr(uint32_t addr, uint32_t value)
{
  volatile uint32_t *ptr = (uint32_t *)addr;
  *ptr = value;
}

static inline void
reg_wr8(uint32_t addr, uint8_t value)
{
  volatile uint8_t *ptr = (uint8_t *)addr;
  *ptr = value;
}

static inline void
reg_wr16(uint32_t addr, uint16_t value)
{
  volatile uint16_t *ptr = (uint16_t *)addr;
  *ptr = value;
}

static inline uint32_t
reg_rd(uint32_t addr)
{
  volatile uint32_t *ptr = (uint32_t *)addr;
  return *ptr;
}

static inline uint16_t
reg_rd16(uint32_t addr)
{
  volatile uint16_t *ptr = (uint16_t *)addr;
  return *ptr;
}

static inline uint8_t
reg_rd8(uint32_t addr)
{
  volatile uint8_t *ptr = (uint8_t *)addr;
  return *ptr;
}
