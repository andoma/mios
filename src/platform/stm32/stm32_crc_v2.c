// This file is not compiled on its own but needs to be included
// by a stm32 chip specific file
#include <stdio.h>
#include <unistd.h>
#include <mios/task.h>
#include "util/crc32.h"

#define CRC_DR   0x00
#define CRC_IDR  0x04
#define CRC_CR   0x08
#define CRC_INIT 0x10
#define CRC_POL  0x14

static mutex_t crcmutex = MUTEX_INITIALIZER("crc");

// There's no __ARM_FEATURE flag for rbit but it seems
// to go hand in hand with clz

#ifdef __ARM_FEATURE_CLZ

static uint32_t
bitrev(uint32_t in)
{
  uint32_t out;
  asm("rbit %0, %1" : "=r" (out) : "r" (in) );
  return out;
}

#else

static uint32_t
bitrev(uint32_t b)
{
  uint32_t mask = 0b11111111111111110000000000000000;
  b = (b & mask) >> 16 | (b & ~mask) << 16;
  mask = 0b11111111000000001111111100000000;
  b = (b & mask) >> 8 | (b & ~mask) << 8;
  mask = 0b11110000111100001111000011110000;
  b = (b & mask) >> 4 | (b & ~mask) << 4;
  mask = 0b11001100110011001100110011001100;
  b = (b & mask) >> 2 | (b & ~mask) << 2;
  mask = 0b10101010101010101010101010101010;
  b = (b & mask) >> 1 | (b & ~mask) << 1;
  return b;
}

#endif


uint32_t
crc32(uint32_t in, const void *data, size_t len)
{
  size_t i = 0;
  const uint8_t *d8 = data;

  in = in ? bitrev(~in) : -1;

  mutex_lock(&crcmutex);
  clk_enable(CLK_CRC);

  reg_wr(CRC_BASE + CRC_INIT, in);
  reg_wr(CRC_BASE + CRC_CR, 0xa1);

  for(i = 0; i + 4 <= len; i += 4)
    reg_wr(CRC_BASE + CRC_DR, __builtin_bswap32(*(uint32_t *)&d8[i]));

  for(; i + 2 <= len; i += 2)
    reg_wr16(CRC_BASE + CRC_DR, __builtin_bswap16(*(uint16_t *)&d8[i]));

  for(; i < len; i++)
    reg_wr8(CRC_BASE + CRC_DR, d8[i]);

  uint32_t r = reg_rd(CRC_BASE + CRC_DR);
  clk_disable(CLK_CRC);
  mutex_unlock(&crcmutex);
  return ~r;
}
