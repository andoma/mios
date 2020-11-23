#include <stdint.h>
#include <stdlib.h>

#include "cpu.h"

// XXX: This is a bad PRNG as it only uses the SYSTICK timer as source
// and optionally cpu_cycle_counter() if TASK_ACCOUNTING happens to be enabled

// PRNG from http://burtleburtle.net/bob/rand/smallprng.html (Public Domain)

#define rot(x,k) (((x)<<(k))|((x)>>(32-(k))))

typedef struct { uint32_t a; uint32_t b; uint32_t c; uint32_t d; } prng_t;


static inline uint32_t
prng_get(prng_t *x, uint32_t seed)
{
  uint32_t e = x->a - rot(x->b, 27);
  x->a = x->b ^ rot(x->c, 17) ^ seed;
  x->b = x->c + x->d;
  x->c = x->d + e;
  x->d = e + x->a;
  return x->d;
}



int  __attribute__((weak))
rand(void)
{
  static volatile unsigned int * const SYST_VAL = (unsigned int *)0xe000e018;
  static prng_t state;

  uint32_t src = *SYST_VAL;
#ifdef TASK_ACCOUNTING
  src ^= cpu_cycle_counter();
#endif

  return prng_get(&state, src) & RAND_MAX;
}


#include <mios/cli.h>
static int
cmd_rand(cli_t *cli, int argc, char **argv)
{
  cli_printf(cli, "%d\n", rand());
  return 0;
}

CLI_CMD_DEF("rand", cmd_rand);
