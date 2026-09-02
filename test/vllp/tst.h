/*
 * Minimal test support: timestamps, logging, failure accounting.
 */
#pragma once

#include <stdint.h>
#include <stddef.h>

int64_t tst_now_us(void);

void tst_sleep_us(int64_t us);

/* Timestamped line to stderr (and the run log, if any). */
void tst_logf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

/* Only printed with -v */
void tst_verbosef(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

void tst_set_verbose(int on);

/* Record a failure for the current scenario. Never aborts. */
void tst_failf(const char *file, int line, const char *fmt, ...)
  __attribute__((format(printf, 3, 4)));

#define TST_CHECK(cond, ...) \
  do { if(!(cond)) tst_failf(__FILE__, __LINE__, __VA_ARGS__); } while(0)

#define TST_FAIL(...) tst_failf(__FILE__, __LINE__, __VA_ARGS__)

/* Scenario bookkeeping */
void tst_scenario_begin(const char *name);
int tst_scenario_end(void);       /* returns failures in this scenario */
int tst_failures(void);           /* failures in current scenario */
int tst_total_failures(void);

/* Simple xorshift RNG so runs are reproducible per seed */
typedef struct { uint64_t s; } tst_rng_t;
void tst_rng_seed(tst_rng_t *r, uint64_t seed);
uint32_t tst_rng_u32(tst_rng_t *r);
double tst_rng_unit(tst_rng_t *r);    /* [0,1) */
