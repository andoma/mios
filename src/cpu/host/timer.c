/*
 * Clock and timers.
 *
 * Real time (default): clock_get() is CLOCK_MONOTONIC in microseconds
 * since process start. Timers are tickless: one POSIX timer is armed
 * for the earliest deadline in the queue and delivers SIGALRM, which
 * runs timer_dispatch() at IRQ_LEVEL_CLOCK like exc_systick() does.
 *
 * Virtual time (test suites): the clock is a variable that only moves
 * when the CPU is idle, and then jumps straight to the next timer
 * deadline. A test that sleeps for a minute takes microseconds, and
 * every run interleaves identically. A thread that never blocks starves
 * time, which is the intended semantics of a closed world.
 */

#include <stdint.h>
#include <unistd.h>

#include <mios/mios.h>
#include <mios/timer.h>

#include "irq.h"
#include "linux.h"

static struct timer_list timers;

static uint64_t boot_ns;
static int host_timer_id;
static int timer_irq;

// Set before init() clears .bss, hence .data
int host_vtime __attribute__((section(".data")));
static uint64_t vclock;


uint64_t
clock_get_irq_blocked(void)
{
  if(host_vtime)
    return vclock;
  return (linux_clock_gettime_ns(CLOCK_MONOTONIC) - boot_ns) / 1000;
}


uint64_t
clock_get(void)
{
  return clock_get_irq_blocked();
}


void
udelay(unsigned int usec)
{
  if(host_vtime) {
    vclock += usec;
    return;
  }
  const uint64_t deadline = clock_get_irq_blocked() + usec;
  while(clock_get_irq_blocked() < deadline) {}
}


static void
timer_program(void)
{
  if(host_vtime)
    return;

  const timer_t *t = LIST_FIRST(&timers);
  struct linux_itimerspec its = {};

  if(t != NULL) {
    const uint64_t ns = t->t_expire * 1000 + boot_ns;
    its.it_value.tv_sec = ns / 1000000000ull;
    its.it_value.tv_nsec = ns % 1000000000ull;
    if(its.it_value.tv_sec == 0 && its.it_value.tv_nsec == 0)
      its.it_value.tv_nsec = 1; // {0,0} would disarm
  }
  linux_syscall(SYS_timer_settime, host_timer_id, TIMER_ABSTIME, &its, NULL);
}


// IRQ_LEVEL_CLOCK must be blocked
void
timer_arm_abs(timer_t *t, uint64_t expire)
{
  timer_arm_on_queue(t, expire, &timers);
  if(LIST_FIRST(&timers) == t)
    timer_program();
}


static void
timer_irq_handler(void *arg)
{
  timer_dispatch(&timers, clock_get_irq_blocked());
  timer_program();
}


// Virtual time helpers for the coordinator in sim.c

uint64_t
host_timer_next(void)
{
  int q = irq_forbid(IRQ_LEVEL_CLOCK);
  const timer_t *t = LIST_FIRST(&timers);
  const uint64_t r = t ? t->t_expire : UINT64_MAX;
  irq_permit(q);
  return r;
}

void
host_vclock_set(uint64_t now)
{
  if(now > vclock)
    vclock = now;
}

void
host_timer_fire(void)
{
  host_irq_raise(timer_irq);
}


static void __attribute__((constructor(101)))
host_timer_init(void)
{
  boot_ns = linux_clock_gettime_ns(CLOCK_MONOTONIC);

  timer_irq = host_irq_alloc(IRQ_LEVEL_CLOCK, timer_irq_handler, NULL);
  host_irq_attach_timer(timer_irq);

  if(host_vtime)
    return;

  struct linux_sigevent sev = {
    .sigev_value = timer_irq,
    .sigev_signo = HOST_SIG_TIMER,
    .sigev_notify = SIGEV_THREAD_ID,
    .sigev_tid = linux_syscall(SYS_gettid),
  };
  if(linux_syscall(SYS_timer_create, CLOCK_MONOTONIC, &sev, &host_timer_id))
    panic("timer_create failed");
}
