/*
 * "hosttest": exercise the scheduler on the host CPU port.
 *
 * The interesting part of the port is preemption: a timer signal must
 * be able to interrupt a running thread, run the kernel's task_switch()
 * inside the signal handler and resume some other thread. A pure
 * cooperative test would pass even if that were broken, so test 1 pins
 * a busy-looping low priority thread and checks that a higher priority
 * thread sleeping on the clock still wakes up on time.
 */

#include <stdio.h>
#include <unistd.h>
#include <string.h>

#include <mios/cli.h>
#include <mios/task.h>
#include <mios/error.h>

#include "hosttest.h"

// ---- 1. Preemption of a spinning thread by a timer wakeup ----

static volatile int spin_stop;
static volatile uint64_t spin_count;

static void *
spinner(void *arg)
{
  while(!spin_stop)
    spin_count++;
  return NULL;
}

static int
test_preempt(void)
{
  spin_stop = 0;
  spin_count = 0;

  thread_t *t = thread_create(spinner, NULL, 0, "spin", 0, 1);

  const int rounds = 50;
  const int period = 1000; // us
  uint64_t worst = 0;
  const uint64_t t0 = clock_get();
  for(int i = 0; i < rounds; i++) {
    const uint64_t a = clock_get();
    usleep(period);
    const uint64_t lat = clock_get() - a - period;
    if(lat > worst)
      worst = lat;
  }
  const uint64_t total = clock_get() - t0;

  spin_stop = 1;
  thread_join(t);

  const int ok = total < (uint64_t)rounds * period * 5 && spin_count > 0;
  hosttest_log("preempt:   %s  %d x usleep(%d) took %d us, worst late %d us, spinner ran %d loops",
             ok ? "PASS" : "FAIL", rounds, period, (int)total, (int)worst,
             (int)spin_count);
  return ok;
}

// ---- 2. Mutex + condvar ping-pong between two threads ----

static mutex_t pp_mutex = MUTEX_INITIALIZER("pingpong");
static cond_t pp_cond;
static int pp_turn;
static int pp_count[2];

#define PP_ROUNDS 20000

static void *
pingpong(void *arg)
{
  const int me = (intptr_t)arg;
  for(int i = 0; i < PP_ROUNDS; i++) {
    mutex_lock(&pp_mutex);
    while(pp_turn != me)
      cond_wait(&pp_cond, &pp_mutex);
    pp_count[me]++;
    pp_turn = !me;
    cond_signal(&pp_cond);
    mutex_unlock(&pp_mutex);
  }
  return NULL;
}

static int
test_pingpong(void)
{
  pp_turn = 0;
  pp_count[0] = pp_count[1] = 0;
  cond_init(&pp_cond, "pingpong");

  const uint64_t t0 = clock_get();
  thread_t *a = thread_create(pingpong, (void *)0, 0, "ping", 0, 3);
  thread_t *b = thread_create(pingpong, (void *)1, 0, "pong", 0, 3);
  thread_join(a);
  thread_join(b);
  const uint64_t total = clock_get() - t0;

  const int ok = pp_count[0] == PP_ROUNDS && pp_count[1] == PP_ROUNDS;
  hosttest_log("pingpong:  %s  %d+%d handovers in %d us",
             ok ? "PASS" : "FAIL", pp_count[0], pp_count[1], (int)total);
  return ok;
}

// ---- 3. Thread arguments (register passing in cpu_stack_init) ----

static volatile long args_result;

static void *
args_thread(long a, long b, long c, long d)
{
  args_result = a * 1000000 + b * 10000 + c * 100 + d;
  return NULL;
}

static int
test_args(void)
{
  args_result = 0;
  thread_t *t = thread_createv(args_thread, 0, "args", 0, 3, 11, 22, 33, 44);
  thread_join(t);
  const int ok = args_result == 11223344;
  hosttest_log("args:      %s  got %ld\n", ok ? "PASS" : "FAIL", args_result);
  return ok;
}

// ---- 4. Timed waits ----

static int
test_timeout(void)
{
  mutex_t m = MUTEX_INITIALIZER("timeout");
  cond_t c;
  cond_init(&c, "timeout");

  mutex_lock(&m);
  const uint64_t t0 = clock_get();
  int r = cond_wait_timeout(&c, &m, t0 + 20000);
  const uint64_t took = clock_get() - t0;
  mutex_unlock(&m);

  const int ok = r == 1 && took >= 20000 && took < 100000;
  hosttest_log("timeout:   %s  cond_wait_timeout(20ms) -> %d (1=timeout) after %d us",
             ok ? "PASS" : "FAIL", r, (int)took);
  return ok;
}


static int
run_sched(void)
{
  int fails = 0;
  fails += !test_args();
  fails += !test_timeout();
  fails += !test_pingpong();
  fails += !test_preempt();
  return fails;
}

// The preempt test needs a spinning thread to be interrupted by the
// clock, which only happens in real time.
HOSTTEST_SUITE("sched", run_sched, HOSTTEST_REALTIME);

static error_t
cmd_hosttest(cli_t *cli, int argc, char **argv)
{
  int fails = run_sched();
  hosttest_log("hosttest: %s", fails ? "FAILED" : "ALL PASS");
  return fails ? ERR_OPERATION_FAILED : 0;
}

CLI_CMD_DEF("hosttest", cmd_hosttest);
