#include <mios/profile.h>
#include <mios/cli.h>
#include <mios/mios.h>
#include <mios/stream.h>

#include <malloc.h>
#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

// Linker symbols. Weak so platforms that don't define them all still link.
// Range is [start, end); skipped if start >= end.
extern char _stext[]      __attribute__((weak));
extern char _text_end[]   __attribute__((weak));
extern char _sfastcode[]  __attribute__((weak));
extern char _efastcode[]  __attribute__((weak));

#define BUCKET_SHIFT  4
#define BUCKET_SIZE   (1u << BUCKET_SHIFT)
#define BUCKET_MAX    0xffffu

struct profile_range {
  const char *name;
  char       *start;
  char       *end;
  uint16_t   *buckets;
  size_t      bucket_count;
};

static struct profile_range g_ranges[] = {
  { "fastcode", _sfastcode, _efastcode, NULL, 0 },
  { "text",     _stext,     _text_end,  NULL, 0 },
};

#define NUM_RANGES (sizeof(g_ranges) / sizeof(g_ranges[0]))

static uint32_t profile_total_samples;
static uint32_t profile_out_of_range;

void
profile_sample(uint32_t pc)
{
  profile_total_samples++;
  for(unsigned i = 0; i < NUM_RANGES; i++) {
    struct profile_range *r = &g_ranges[i];
    if(r->buckets == NULL)
      continue;
    if(pc < (uintptr_t)r->start || pc >= (uintptr_t)r->end)
      continue;
    size_t idx = (pc - (uintptr_t)r->start) >> BUCKET_SHIFT;
    if(r->buckets[idx] < BUCKET_MAX)
      r->buckets[idx]++;
    return;
  }
  profile_out_of_range++;
}

static void __attribute__((constructor(140)))
profile_init(void)
{
  for(unsigned i = 0; i < NUM_RANGES; i++) {
    struct profile_range *r = &g_ranges[i];
    if(r->start >= r->end)
      continue;
    size_t span = r->end - r->start;
    r->bucket_count = (span + BUCKET_SIZE - 1) >> BUCKET_SHIFT;
    r->buckets = xalloc(r->bucket_count * sizeof(uint16_t), 0, 0);
    memset(r->buckets, 0, r->bucket_count * sizeof(uint16_t));
  }
}

static error_t
cmd_profile_clear(cli_t *cli, int argc, char **argv)
{
  for(unsigned i = 0; i < NUM_RANGES; i++) {
    struct profile_range *r = &g_ranges[i];
    if(r->buckets != NULL)
      memset(r->buckets, 0, r->bucket_count * sizeof(uint16_t));
  }
  return 0;
}

struct profile_entry {
  uint32_t addr;
  uint32_t count;
};

static error_t
cmd_profile_dump(cli_t *cli, int argc, char **argv)
{
  cli_printf(cli, "# samples=%u out-of-range=%u\n",
             profile_total_samples, profile_out_of_range);

  // Count non-zero buckets across all ranges.
  size_t n = 0;
  for(unsigned i = 0; i < NUM_RANGES; i++) {
    struct profile_range *r = &g_ranges[i];
    if(r->buckets == NULL)
      continue;
    for(size_t j = 0; j < r->bucket_count; j++)
      if(r->buckets[j] != 0)
        n++;
  }
  if(n == 0)
    return 0;

  struct profile_entry *arr = xalloc(n * sizeof(*arr), 0, 0);
  if(arr == NULL) {
    cli_printf(cli, "# alloc failed for %u entries\n", (unsigned)n);
    return ERR_NO_MEMORY;
  }

  // Compact non-zero buckets into the array.
  size_t k = 0;
  for(unsigned i = 0; i < NUM_RANGES; i++) {
    struct profile_range *r = &g_ranges[i];
    if(r->buckets == NULL)
      continue;
    for(size_t j = 0; j < r->bucket_count; j++) {
      uint16_t c = r->buckets[j];
      if(c == 0)
        continue;
      arr[k].addr  = (uintptr_t)r->start + (j << BUCKET_SHIFT);
      arr[k].count = c;
      k++;
    }
  }

  // Selection sort the top-20 entries by count, descending. We only
  // care about the top of the list, so don't bother sorting the rest.
  size_t top = n < 20 ? n : 20;
  for(size_t i = 0; i < top; i++) {
    size_t best = i;
    for(size_t j = i + 1; j < n; j++)
      if(arr[j].count > arr[best].count)
        best = j;
    if(best != i) {
      struct profile_entry tmp = arr[i];
      arr[i] = arr[best];
      arr[best] = tmp;
    }
    cli_printf(cli, "0x%08x %u\n", (unsigned)arr[i].addr, (unsigned)arr[i].count);
  }

  free(arr);
  return 0;
}

CLI_CMD_DEF("profile_clear", cmd_profile_clear);
CLI_CMD_DEF("profile_dump",  cmd_profile_dump);
