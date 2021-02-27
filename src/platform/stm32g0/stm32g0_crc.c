#include <stdio.h>
#include <stdlib.h>
#include <mios/task.h>
#include "net/crc32.h"

#include "stm32g0.h"
#include "stm32g0_clk.h"

#define CRC_BASE 0x40023000

#define CRC_DR   0x00
#define CRC_IDR  0x04
#define CRC_CR   0x08
#define CRC_INIT 0x10
#define CRC_POL  0x14

static volatile uint8_t  *const CRC_DR8  = (volatile uint8_t *)0x40023000;
static volatile uint32_t *const CRC_DR32 = (volatile uint32_t *)0x40023000;
static mutex_t crcmutex = MUTEX_INITIALIZER("crc");

uint32_t
crc32(const void *data, size_t len)
{
  size_t i;
  const uint8_t *d8 = data;

  mutex_lock(&crcmutex);

  clk_enable(CLK_CRC);

  reg_wr(CRC_BASE + CRC_CR, 0xe1);

  for(i = 0; i + 4 < len; i += 4)
    *CRC_DR32 = *(uint32_t *)&d8[i];

  for(; i < len; i++)
    *CRC_DR8 = d8[i];

  uint32_t r = reg_rd(CRC_BASE + CRC_DR);
  clk_disable(CLK_CRC);
  mutex_unlock(&crcmutex);
  return ~r;
}
