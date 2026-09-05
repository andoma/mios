#pragma once

/*
 * Test suites for the host platform.
 *
 *   build.host/mios.elf <suite> [--seed=N]     run one suite, exit 0/1
 *   build.host/mios.elf --list                 list suites
 *
 * A suite is a function returning the number of failed checks. Suites
 * run in the main thread with the whole kernel up, so they can create
 * threads, netifs and so on like any Mios code. Unless flagged
 * HOSTTEST_REALTIME they run in virtual time: the clock jumps to the
 * next timer whenever the CPU is idle, so sleeps are free and runs are
 * deterministic. A suite that needs a busy-looping thread to be
 * preempted by a timer must be realtime (time does not move while
 * something is runnable).
 */

#include <stdint.h>
#include <stddef.h>

#include <mios/mios.h>

#define HOSTTEST_REALTIME 0x1

typedef struct hosttest_suite {
  const char *name;
  int (*run)(void);
  uint32_t flags;
  // Pbuf buffer size this suite needs, or 0 for the platform default.
  // Declared rather than set at run time because the pool is carved
  // before any suite starts. Lets one binary cover both a roomy pool and
  // the tight one a constrained target uses, which matters because a
  // transmit path that is fine against 512 can overrun 72.
  uint16_t pbuf_data_size;
} hosttest_suite_t;

#define HOSTTEST_SUITE_EX(name, fn, flags, pbufsz)                      \
  static const hosttest_suite_t MIOS_JOIN(hosttest_, __LINE__)          \
  __attribute__((used, section("hosttest." name))) =                    \
  { name, fn, flags, pbufsz }

#define HOSTTEST_SUITE(name, fn, flags) HOSTTEST_SUITE_EX(name, fn, flags, 0)

// Log with a timestamp prefix
void hosttest_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

// Returns !!cond, prints "CHECK FAILED file:line: msg" when false
int hosttest_check(int cond, const char *file, int line,
                   const char *fmt, ...) __attribute__((format(printf, 4, 5)));

#define CHECK(cond, fmt...) hosttest_check(!!(cond), __FILE__, __LINE__, fmt)

// Poll pred(arg) every 10ms until true or timeout (us). Returns 1 if true.
int hosttest_wait(int (*pred)(void *arg), void *arg, uint64_t timeout);
