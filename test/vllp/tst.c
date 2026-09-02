#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "tst.h"

#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <time.h>
#include <pthread.h>
#include <unistd.h>

static int g_verbose;
static int g_failures;
static int g_total_failures;
static const char *g_scenario = "";
static int64_t g_scenario_t0;
static pthread_mutex_t g_mutex = PTHREAD_MUTEX_INITIALIZER;

int64_t
tst_now_us(void)
{
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (int64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}

void
tst_sleep_us(int64_t us)
{
  if(us <= 0)
    return;
  struct timespec ts = { us / 1000000, (us % 1000000) * 1000 };
  while(nanosleep(&ts, &ts) < 0)
    ;
}

static void
vlog(const char *pfx, const char *fmt, va_list ap)
{
  char line[1024];
  vsnprintf(line, sizeof(line), fmt, ap);
  int64_t t = tst_now_us() - g_scenario_t0;
  pthread_mutex_lock(&g_mutex);
  fprintf(stderr, "[%7.3f] %s%s\n", t / 1e6, pfx, line);
  fflush(stderr);
  pthread_mutex_unlock(&g_mutex);
}

void
tst_logf(const char *fmt, ...)
{
  va_list ap;
  va_start(ap, fmt);
  vlog("", fmt, ap);
  va_end(ap);
}

void
tst_verbosef(const char *fmt, ...)
{
  if(!g_verbose)
    return;
  va_list ap;
  va_start(ap, fmt);
  vlog("  ", fmt, ap);
  va_end(ap);
}

void
tst_set_verbose(int on)
{
  g_verbose = on;
}

void
tst_failf(const char *file, int line, const char *fmt, ...)
{
  char msg[512];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(msg, sizeof(msg), fmt, ap);
  va_end(ap);
  __sync_add_and_fetch(&g_failures, 1);
  __sync_add_and_fetch(&g_total_failures, 1);
  tst_logf("FAIL %s: %s (%s:%d)", g_scenario, msg, file, line);
}

void
tst_scenario_begin(const char *name)
{
  g_scenario = name;
  g_failures = 0;
  g_scenario_t0 = tst_now_us();
  tst_logf("=== %s", name);
}

int
tst_scenario_end(void)
{
  int f = g_failures;
  tst_logf("=== %s: %s (%d failure%s, %.2fs)", g_scenario,
           f ? "FAIL" : "PASS", f, f == 1 ? "" : "s",
           (tst_now_us() - g_scenario_t0) / 1e6);
  return f;
}

int
tst_failures(void)
{
  return g_failures;
}

int
tst_total_failures(void)
{
  return g_total_failures;
}

void
tst_rng_seed(tst_rng_t *r, uint64_t seed)
{
  r->s = seed ? seed : 0x9e3779b97f4a7c15ULL;
}

uint32_t
tst_rng_u32(tst_rng_t *r)
{
  uint64_t x = r->s;
  x ^= x << 13;
  x ^= x >> 7;
  x ^= x << 17;
  r->s = x;
  return (uint32_t)(x >> 16);
}

double
tst_rng_unit(tst_rng_t *r)
{
  return tst_rng_u32(r) / 4294967296.0;
}
