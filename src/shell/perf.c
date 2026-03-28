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


static void
memtest_region(cli_t *cli, void *buf, size_t size, const char *name)
{
  volatile uint32_t *p = buf;
  size_t words = size / sizeof(uint32_t);
  int64_t start, stop;
  size_t rounds;

  // Write test
  start = clock_get();
  rounds = 0;
  while(clock_get() - start < 1000000) {
    for(size_t i = 0; i < words; i++)
      p[i] = i;
    rounds++;
  }
  stop = clock_get();
  uint64_t write_bps = (uint64_t)size * rounds * 1000000 / (stop - start);

  // Read test
  start = clock_get();
  rounds = 0;
  volatile uint32_t sink;
  while(clock_get() - start < 1000000) {
    for(size_t i = 0; i < words; i++)
      sink = p[i];
    rounds++;
  }
  stop = clock_get();
  (void)sink;
  uint64_t read_bps = (uint64_t)size * rounds * 1000000 / (stop - start);

  cli_printf(cli, "  %-12s %p  W:%ld  R:%ld MB/s\n",
             name, buf,
             (long)(write_bps / (1024 * 1024)),
             (long)(read_bps / (1024 * 1024)));
}


static error_t
cmd_memperf(cli_t *cli, int argc, char **argv)
{
  const size_t size = 32768;

  static const struct {
    const char *name;
    unsigned int type;
  } regions[] = {
    { "LOCAL",  MEM_TYPE_LOCAL | MEM_MAY_FAIL },
    { "DMA",    MEM_TYPE_DMA | MEM_MAY_FAIL },
    { "General", MEM_MAY_FAIL },
  };

  for(size_t i = 0; i < sizeof(regions) / sizeof(regions[0]); i++) {
    void *buf = xalloc(size, 0, regions[i].type);
    if(buf == NULL) {
      cli_printf(cli, "  %-12s (not available)\n", regions[i].name);
      continue;
    }
    memtest_region(cli, buf, size, regions[i].name);
    free(buf);
  }
  return 0;
}

CLI_CMD_DEF("memperf", cmd_memperf);

