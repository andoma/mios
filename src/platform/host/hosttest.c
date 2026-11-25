#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <mios/cli.h>
#include <mios/task.h>

#include "hosttest.h"
#include "cpu.h"

extern const hosttest_suite_t _hosttest_array_begin[];
extern const hosttest_suite_t _hosttest_array_end[];

static int hosttest_fails;

// Set once a suite is running. panic() normally drops into an
// interactive crash shell, which is what you want on a target but spins
// on EOF in a batch run -- so a panic inside a suite would hang the run
// instead of failing it. See panic_enter_console() below.
static int hosttest_running;


static const hosttest_suite_t *
hosttest_find(const char *name)
{
  for(const hosttest_suite_t *s = _hosttest_array_begin;
      s != _hosttest_array_end; s++) {
    if(!strcmp(s->name, name))
      return s;
  }
  return NULL;
}


// cpu layer asks before boot whether to run the clock virtually
int
host_platform_vtime(const char *suite)
{
  const hosttest_suite_t *s = hosttest_find(suite);
  return s != NULL && !(s->flags & HOSTTEST_REALTIME);
}


// cpu layer asks before boot how big this suite wants its pbufs
int
host_platform_pbuf_data_size(const char *suite)
{
  const hosttest_suite_t *s = hosttest_find(suite);
  return s != NULL ? s->pbuf_data_size : 0;
}


// ...and how many of them
int
host_platform_pbuf_pool_count(const char *suite)
{
  const hosttest_suite_t *s = hosttest_find(suite);
  return s != NULL ? s->pbuf_pool_count : 0;
}


// Overrides the weak default in kernel/panic.c: a panic terminates the
// process (halt() exits non-zero) unless someone at a terminal can use the
// crash shell. Not while a suite runs, and not with stdin a pipe or a file
// (a harness running an app, `echo cmd | mios`), where the shell would wait
// for input that never comes and hide the crash from whoever runs it.
int
panic_enter_console(void)
{
  return !hosttest_running && host_console_is_tty();
}


void
hosttest_log(const char *fmt, ...)
{
  const uint64_t now = clock_get();
  printf("[%4d.%06d] ", (int)(now / 1000000), (int)(now % 1000000));
  va_list ap;
  va_start(ap, fmt);
  vstprintf(stdio, fmt, ap);
  va_end(ap);
  printf("\n");
}


int
hosttest_check(int cond, const char *file, int line, const char *fmt, ...)
{
  if(cond)
    return 1;
  hosttest_fails++;
  const uint64_t now = clock_get();
  printf("[%4d.%06d] CHECK FAILED %s:%d: ", (int)(now / 1000000),
         (int)(now % 1000000), file, line);
  va_list ap;
  va_start(ap, fmt);
  vstprintf(stdio, fmt, ap);
  va_end(ap);
  printf("\n");
  return 0;
}


int
hosttest_wait(int (*pred)(void *arg), void *arg, uint64_t timeout)
{
  const uint64_t deadline = clock_get() + timeout;
  while(!pred(arg)) {
    if(clock_get() >= deadline)
      return 0;
    usleep(10000);
  }
  return 1;
}


// A suite named on the command line (or --list) takes over the process;
// otherwise main() runs, an app's own or the kernel's shell. A constructor
// after multitasking_mark (kernel/mios.c), so it runs on the main thread
// just before main(); not 65535, the default priority, whose section the
// host linker scripts do not keep.
static void __attribute__((constructor(9999)))
hosttest_entry(void)
{
  if(host_arg("list") != NULL || host_positional(0) != NULL)
    hosttest_main();
}


void
hosttest_main(void)
{
  if(host_arg("list") != NULL) {
    for(const hosttest_suite_t *s = _hosttest_array_begin;
        s != _hosttest_array_end; s++) {
      printf("%s%s\n", s->name,
             s->flags & HOSTTEST_REALTIME ? " (realtime)" : "");
    }
    host_exit(0);
  }

  const char *name = host_positional(0);
  if(name == NULL)
    return;

  const hosttest_suite_t *s = hosttest_find(name);
  if(s == NULL) {
    printf("hosttest: unknown suite '%s' (try --list)\n", name);
    host_exit(2);
  }

  const char *seedstr = host_arg("seed");
  const uint32_t seed = seedstr && *seedstr ? atoi(seedstr) : 1;
  host_rand_seed(seed);

  printf("hosttest: suite '%s', %s time, seed %u\n", s->name,
         host_vtime ? "virtual" : "real", seed);

  const uint64_t t0 = clock_get();
  hosttest_running = 1;
  int fails = s->run();
  hosttest_running = 0;
  fails += hosttest_fails;
  const uint64_t elapsed = clock_get() - t0;

  if(fails) {
    printf("\n---- eventlog ----\n");
    cli_t cli = { stdio };
    char cmd[] = "log";
    cli_dispatch(&cli, cmd);
    printf("\nhosttest: %s FAILED (%d checks) after %d.%03d s %s time\n",
           s->name, fails, (int)(elapsed / 1000000),
           (int)(elapsed / 1000 % 1000), host_vtime ? "virtual" : "real");
    host_exit(1);
  }
  printf("hosttest: %s PASS after %d.%03d s %s time\n", s->name,
         (int)(elapsed / 1000000), (int)(elapsed / 1000 % 1000),
         host_vtime ? "virtual" : "real");
  host_exit(0);
}
