#include <unistd.h>
#include <math.h>
#include <mios/cli.h>


static error_t
cmd_math_test(cli_t *cli, int argc, char **argv)
{
  float v = argc;
  cli_printf(cli, "Measuring sqrtf(%f) performance for 5 seconds\n", v);
  int64_t stop_at = clock_get() + 5000000;
  int rounds = 0;
  while(clock_get() < stop_at) {
    for(int i = 0; i < 1000; i++) {
      v = sqrtf(v);
      v = sqrtf(v);
      v = sqrtf(v);
      v = sqrtf(v);
      v = sqrtf(v);
      v = sqrtf(v);
      v = sqrtf(v);
      v = sqrtf(v);
      v = sqrtf(v);
      v = sqrtf(v);
    }
    rounds++;
  }

  int ips = rounds * 10000 / 5;
  cli_printf(cli, "%d sqrtf() / second result:%f\n", ips, v);
  return 0;
}

CLI_CMD_DEF("mathtest", cmd_math_test);
