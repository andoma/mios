#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <mios/mios.h>

#include "sim.h"
#include "cpu.h"
#include "irq.h"
#include "linux.h"

struct sim_thread {
  struct sim_thread *next;
  const char *name;
  void (*fn)(void *arg);
  void *arg;
  void *stack_lo;
  void *stack_hi;

  volatile uint32_t run;      // futex: coordinator -> peer, "you may run"
  volatile uint32_t posted;   // an event is waiting for this peer
  volatile uint32_t dead;
  uint64_t deadline;          // when waiting; SIM_NEVER for event-only
};

static sim_thread_t *sim_threads;      // creation order
static sim_thread_t **sim_threads_tail = &sim_threads;

static volatile uint32_t coord_wake;   // futex: peer -> coordinator, "your turn"


sim_thread_t *
sim_current(void)
{
  const void *sp = __builtin_frame_address(0);
  for(sim_thread_t *t = sim_threads; t != NULL; t = t->next) {
    if(sp >= t->stack_lo && sp < t->stack_hi)
      return t;
  }
  return NULL;
}


// ---- Handover ----

static void
sim_yield_to_coordinator(void)
{
  __atomic_store_n(&coord_wake, 1, __ATOMIC_SEQ_CST);
  linux_futex_wake(&coord_wake);
}

static void
sim_block_until_run(sim_thread_t *t)
{
  while(__atomic_exchange_n(&t->run, 0, __ATOMIC_SEQ_CST) == 0)
    linux_futex_wait(&t->run, 0);
}

// Coordinator: let t run until it waits again (or dies)
static void
sim_run(sim_thread_t *t)
{
  __atomic_store_n(&t->run, 1, __ATOMIC_SEQ_CST);
  linux_futex_wake(&t->run);
  while(__atomic_exchange_n(&coord_wake, 0, __ATOMIC_SEQ_CST) == 0)
    linux_futex_wait(&coord_wake, 0);
}


int
sim_wait(uint64_t deadline)
{
  sim_thread_t *t = sim_current();
  if(t == NULL)
    panic("sim_wait() called from the Mios CPU thread");

  t->deadline = deadline;
  sim_yield_to_coordinator();
  sim_block_until_run(t);
  t->deadline = SIM_NEVER;
  return __atomic_exchange_n(&t->posted, 0, __ATOMIC_SEQ_CST) != 0;
}


void
sim_post(sim_thread_t *t)
{
  __atomic_store_n(&t->posted, 1, __ATOMIC_SEQ_CST);
}


// ---- Threads ----

static void
sim_thread_main(void *arg)
{
  sim_thread_t *t = arg;
  sim_block_until_run(t);          // first run is scheduled like any other
  t->fn(t->arg);
  t->dead = 1;
  sim_yield_to_coordinator();      // and exit (entry.S)
}


sim_thread_t *
sim_thread_create(const char *name, void (*fn)(void *arg), void *arg,
                  size_t stack_size)
{
  if(!host_vtime)
    panic("sim_thread_create(%s): simulation threads need virtual time", name);

  if(stack_size < 65536)
    stack_size = 65536;

  sim_thread_t *t = calloc(1, sizeof(sim_thread_t));
  t->name = name;
  t->fn = fn;
  t->arg = arg;
  t->deadline = SIM_NEVER;
  t->posted = 1;                   // runnable at creation

  // Guard page below the stack
  const size_t guard = 4096;
  void *mem = linux_mmap(NULL, stack_size + guard, PROT_READ | PROT_WRITE,
                         MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if(mem == NULL)
    panic("sim_thread_create(%s): mmap failed", name);
  linux_syscall(SYS_mprotect, mem, guard, 0);
  t->stack_lo = mem + guard;
  t->stack_hi = mem + guard + stack_size;

  *sim_threads_tail = t;
  sim_threads_tail = &t->next;

  const unsigned long flags = CLONE_VM | CLONE_FS | CLONE_FILES |
    CLONE_SIGHAND | CLONE_THREAD | CLONE_SYSVSEM;
  if(linux_clone_thread(flags, t->stack_hi, sim_thread_main, t) < 0)
    panic("sim_thread_create(%s): clone failed", name);
  return t;
}


// ---- Watchdog ----

/* A livelock (something runnable forever) stops the virtual clock, so
   the "nothing runnable" deadlock check below never trips and neither
   does anything sleeping on the clock: the run would spin until CI
   gives up. Real Mios goes idle within milliseconds (and the idle loop
   feeds the hardware watchdog), so if the coordinator has not found
   everybody waiting for a second, something is stuck.

   The second is process CPU time, not wall clock, so a loaded machine
   does not trip it. The timer signals the Mios CPU thread by tid: a
   process-directed signal could land on a peer, and the panic should
   show the Mios thread that is spinning. */

#define SIM_WATCHDOG_SEC 1

static int sim_watchdog_id = -1;

static void
sim_watchdog_fire(int sig, linux_siginfo_t *si, void *ucontext)
{
  thread_t *t = thread_current();
  const uint64_t now = clock_get();
  panic_frame(ucontext, "virtual time stuck at %d.%06d s: no idle for %d s "
              "of CPU (livelock?) thread:%s", (int)(now / 1000000),
              (int)(now % 1000000), SIM_WATCHDOG_SEC, t ? t->t_name : "?");
}

static void
sim_watchdog_kick(void)
{
  if(sim_watchdog_id < 0)
    return;
  const struct linux_itimerspec its = {
    .it_value.tv_sec = SIM_WATCHDOG_SEC,
  };
  linux_syscall(SYS_timer_settime, sim_watchdog_id, 0, &its, NULL);
}

static void __attribute__((constructor(102)))
sim_watchdog_init(void)
{
  extern int host_lib_mode;  // idles in cpu.c's lib_idle(), never here
  if(!host_vtime || host_lib_mode)
    return;

  linux_sigaction(SIGPROF, sim_watchdog_fire, SA_SIGINFO | SA_ONSTACK, 0);
  struct linux_sigevent sev = {
    .sigev_signo = SIGPROF,
    .sigev_notify = SIGEV_THREAD_ID,
    .sigev_tid = linux_syscall(SYS_gettid),
  };
  int id;
  if(linux_syscall(SYS_timer_create, CLOCK_PROCESS_CPUTIME_ID, &sev, &id))
    panic("sim watchdog: timer_create failed");
  sim_watchdog_id = id;
  sim_watchdog_kick();
}


// ---- Coordinator ----

static int
sim_runnable(const sim_thread_t *t, uint64_t now)
{
  return !t->dead && (t->posted || t->deadline <= now);
}


void
sim_idle(void)
{
  while(1) {
    const uint64_t now = clock_get();

    // Run every peer that has something to do at this instant
    int ran = 0;
    for(sim_thread_t *t = sim_threads; t != NULL; t = t->next) {
      if(sim_runnable(t, now)) {
        sim_run(t);
        ran = 1;
      }
    }

    // Peers may have pended interrupts (injected frames): Mios goes first
    if(__atomic_load_n(&irq_pending, __ATOMIC_SEQ_CST)) {
      irq_dispatch();
      return;
    }
    if(ran)
      continue;   // a peer may have posted another peer

    // Everybody is waiting. Advance the clock to the earliest deadline.
    sim_watchdog_kick();
    uint64_t next = host_timer_next();
    for(sim_thread_t *t = sim_threads; t != NULL; t = t->next) {
      if(!t->dead && t->deadline < next)
        next = t->deadline;
    }
    if(next == SIM_NEVER) {
      printf("\nvirtual time: nothing runnable and no deadlines, deadlock\n");
      host_exit(3);
    }
    host_vclock_set(next);
    if(host_timer_next() <= next)
      host_timer_fire();   // pends the timer IRQ, dispatched next round
  }
}
