#include <stdint.h>
#include <stdlib.h>

#include <mios/prng.h>

#include "cpu.h"

// XXX: This is a bad PRNG as it only uses the SYSTICK timer as source
// and optionally cpu_cycle_counter() if TASK_ACCOUNTING happens to be enabled

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
static error_t
cmd_rand(cli_t *cli, int argc, char **argv)
{
  cli_printf(cli, "%d\n", rand());
  return 0;
}

CLI_CMD_DEF("rand", cmd_rand);
