/*
 * rand() from the true random number generator.
 *
 * What the generator gives is not returned as it is but stirred into
 * the software generator that rand() is without it (see
 * cpu/cortexm/rnd.c), together with the SysTick counter. If the
 * hardware has nothing to give (not ready in time, or it reports an
 * error) rand() is then no worse than that one, and it never waits for
 * long. How often that happens is counted in stm32n6_rng_stats.
 */

#include <stdint.h>
#include <stdlib.h>

#include <mios/prng.h>

#include "stm32n6_clk.h"
#include "stm32n6_rng.h"

#define RNG_BASE 0x54020000

#define RNG_CR   (RNG_BASE + 0x00)
#define RNG_SR   (RNG_BASE + 0x04)
#define RNG_DR   (RNG_BASE + 0x08)

#define RNG_CR_RNGEN   (1 << 2)
#define RNG_CR_CONDRST (1 << 30)

#define RNG_SR_DRDY    (1 << 0)
#define RNG_SR_CECS    (1 << 1) // Clock error, now
#define RNG_SR_SECS    (1 << 2) // Seed error, now
#define RNG_SR_CEIS    (1 << 5) // Clock error seen
#define RNG_SR_SEIS    (1 << 6) // Seed error seen

// Register reads to wait for the generator. It refills in a few
// microseconds when all is well
#define RNG_POLLS 2000

stm32n6_rng_stats_t stm32n6_rng_stats;

static uint8_t rng_started;


// Reset the conditioning stage and (re)start. The configuration is
// left at its reset value, with the clock error detection on
static void
rng_start(void)
{
  clk_enable(CLK_RNG);

  reg_wr(RNG_CR, reg_rd(RNG_CR) & ~RNG_CR_RNGEN);
  reg_wr(RNG_CR, reg_rd(RNG_CR) | RNG_CR_CONDRST);
  reg_wr(RNG_CR, reg_rd(RNG_CR) & ~RNG_CR_CONDRST);
  for(int i = 0; i < RNG_POLLS; i++) {
    if(!(reg_rd(RNG_CR) & RNG_CR_CONDRST))
      break;
  }
  reg_wr(RNG_CR, reg_rd(RNG_CR) | RNG_CR_RNGEN);
  rng_started = 1;
}


// Returns nonzero and a value if the generator had one
static int
rng_get(uint32_t *value)
{
  if(!rng_started)
    rng_start();

  for(int i = 0; i < RNG_POLLS; i++) {
    const uint32_t sr = reg_rd(RNG_SR);

    if(sr & (RNG_SR_SEIS | RNG_SR_CEIS)) {
      // What is in the data register cannot be trusted after a seed
      // error. The "seen" bits are cleared by writing them as zero;
      // if the error is still there, start over
      stm32n6_rng_stats.errors++;
      reg_wr(RNG_SR, 0);
      if(sr & RNG_SR_SECS)
        rng_start();
      return 0;
    }

    if(sr & RNG_SR_DRDY) {
      *value = reg_rd(RNG_DR);
      if(reg_rd(RNG_SR) & RNG_SR_SEIS)
        return 0; // Counted and dealt with on the next call
      // (Zero is taken to mean "nothing": it is what the data register
      // of other STM32 generators reads when it is not valid)
      if(*value == 0)
        break;
      stm32n6_rng_stats.ok++;
      return 1;
    }
  }
  stm32n6_rng_stats.timeouts++;
  return 0;
}


int
rand(void)
{
  static volatile unsigned int * const SYST_VAL = (unsigned int *)0xe000e018;
  static prng_t state;

  uint32_t src = *SYST_VAL;
  uint32_t hw;
  if(rng_get(&hw))
    src ^= hw;

  return prng_get(&state, src) & RAND_MAX;
}
