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

static inline void
reg_clr(uint32_t addr, uint32_t mask)
{
  volatile uint32_t *ptr = (uint32_t *)addr;
  *ptr &= ~mask;
}

static inline void
reg_set_bits(uint32_t addr, uint32_t shift, uint32_t length, uint32_t bits)
{
  volatile uint32_t *ptr = (uint32_t *)addr;

  const uint32_t mask = ((1 << length) - 1) << shift;

  *ptr = (*ptr & ~mask) | ((bits << shift) & mask);
}



#define FLASH_ACR   0x40023c00


#define RCC_CR      0x40023800
#define RCC_PLLCFGR 0x40023804
#define RCC_CFGR    0x40023808
#define RCC_AHB1ENR 0x40023830
#define RCC_APB1ENR 0x40023840

