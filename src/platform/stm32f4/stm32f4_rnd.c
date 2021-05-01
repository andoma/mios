#include <stdint.h>
#include <stdlib.h>

#include "stm32f4_clk.h"

#define RNG_BASE 0x50060800

#define RNG_CR (RNG_BASE + 0x00)
#define RNG_SR (RNG_BASE + 0x04)
#define RNG_DR (RNG_BASE + 0x08)

int
rand(void)
{
  while(!(reg_rd(RNG_SR) & 1)) {}
  return reg_rd(RNG_DR) & RAND_MAX;
}


static void  __attribute__((constructor(120)))
stm32f4_rng_init(void)
{
  clk_enable(CLK_RNG);
  reg_wr(RNG_CR, 0x4); // RNGEN
}
