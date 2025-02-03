#include <unistd.h>
#include <mios/cli.h>
#include <malloc.h>
#include <stdlib.h>

#include "lib/crypto/sha1.h"

#ifdef ENABLE_MATH
#include <math.h>
#endif

static error_t
cmd_perftest(cli_t *cli, int argc, char **argv)
{
  size_t bufsize = 2048;

  uint8_t *buf = xalloc(bufsize, 0, MEM_MAY_FAIL);
  if(buf == NULL) {
    return ERR_NO_MEMORY;
  }

  cli_printf(cli, "Workmem: %p\n", buf);

  for(size_t i = 0; i < bufsize; i++) {
    buf[i] = rand();
  }


  SHA1_CTX *ctx = (void *)buf;
  SHA1Init(ctx);

  int64_t stop = clock_get() + 1000000;
  size_t rounds = 0;
  size_t sha1bytes = bufsize - sizeof(SHA1_CTX);

  while(clock_get() < stop) {
    for(int i = 0; i < 100; i++) {
      SHA1Update(ctx, buf + sizeof(SHA1_CTX), sha1bytes);
    }
    rounds++;
  }
  cli_printf(cli, "SHA1 bytes/second: %ld\n",
             (long)(sha1bytes * rounds * 100));

#ifdef ENABLE_MATH
  float v = rand();

  // Floating point performance

  stop = clock_get() + 1000000;
  rounds = 0;

  while(clock_get() < stop) {
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

  cli_printf(cli, "sqrtf()/second:    %d  [result:%f]\n", rounds * 10000, v);
#endif
  free(buf);
  return 0;
}

CLI_CMD_DEF("perftest", cmd_perftest);

