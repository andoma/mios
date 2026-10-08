/*
 * "kernel": the scheduler and its synchronisation primitives
 * (src/kernel/task.c) in virtual time.
 *
 * Virtual time makes the scheduler fully deterministic: nothing is
 * preempted by the clock, so a thread only gives up the CPU when it
 * blocks, wakes something of equal or higher priority, or takes an
 * interrupt. That lets most tests here write down the exact order in
 * which threads must run (a trace string, one letter per event) and
 * compare it with what happened. Sleeps cost nothing, so timeouts are
 * checked to the microsecond.
 *
 * The suite runs on the main thread, which is priority 2
 * (thread_create_shell). Most tests lean on that: priority 1 is below
 * us, anything from 3 up preempts us the moment it becomes runnable.
 *
 * The last test is a seeded stress run (--seed=N picks a different
 * interleaving): workers at mixed priorities lock, nest, sleep while
 * holding, wait with timeouts, poll, spawn children and raise an
 * interrupt that wakes a high priority thread in the middle of all
 * that. Every critical section checks it is alone and every counter is
 * reconciled at the end.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <mios/task.h>
#include <mios/timer.h>
#include <mios/poll.h>

#include "hosttest.h"
#include "irq.h"

thread_t *thread_get_next(thread_t *cur);

#define MAIN_PRIO 2

// ---- Trace: one character per event, compared against the expected order

static char trace[128];
static int trace_len;

static void
tr(int c)
{
  if(trace_len < sizeof(trace) - 1)
    trace[trace_len++] = c;
  trace[trace_len] = 0;
}

static void
trace_reset(void)
{
  trace_len = 0;
  trace[0] = 0;
}

#define CHECK_TRACE(want)                                               \
  CHECK(!strcmp(trace, want), "%s: order '%s', want '%s'",              \
        __FUNCTION__, trace, want)

static int
count_waiters(const task_waitable_t *w)
{
  int n = 0;
  const task_t *t;
  LIST_FOREACH(t, &w->list, t_wait_link)
    n++;
  return n;
}

static void *
mark(void *arg)
{
  tr((intptr_t)arg);
  return NULL;
}

static void
check_main_prio(void)
{
  CHECK(thread_current()->t_task.t_prio == MAIN_PRIO,
        "suite runs at prio %d, tests assume %d",
        thread_current()->t_task.t_prio, MAIN_PRIO);
}

// ---- thread_create(): higher prio runs at once, lower waits. Equal
//      prio also runs at once: thread_create() always reschedules, and
//      that puts the caller at the back of its queue, the same "equal
//      prio wakeup yields" rule as task_wakeup() and mutex_unlock()

static void
test_create(void)
{
  trace_reset();
  thread_t *h = thread_create(mark, (void *)'H', 0, "hi", 0, MAIN_PRIO + 3);
  tr('m');
  thread_t *s = thread_create(mark, (void *)'S', 0, "same", 0, MAIN_PRIO);
  tr('m');
  thread_t *l = thread_create(mark, (void *)'L', 0, "lo", 0, MAIN_PRIO - 1);
  tr('m');
  // Blocks until 'L' has run, the only thing left below us
  thread_join(l);
  thread_join(s);
  thread_join(h);
  CHECK_TRACE("HmSmmL");
}

// ---- cond_signal() wakes one, cond_broadcast() wakes all, in priority
//      order with FIFO among equals

static mutex_t wo_mutex = MUTEX_INITIALIZER("wo");
static cond_t wo_cond = COND_INITIALIZER("wo");
static int wo_tickets;

static void *
wo_waiter(void *arg)
{
  mutex_lock(&wo_mutex);
  while(!wo_tickets)
    cond_wait(&wo_cond, &wo_mutex);
  wo_tickets--;
  tr((intptr_t)arg);
  mutex_unlock(&wo_mutex);
  return NULL;
}

static void
test_cond_order(void)
{
  static const struct { char id; uint8_t prio; } w[] = {
    {'a', 3}, {'b', 5}, {'c', 3}, {'d', 4}, {'e', 5}
  };
  thread_t *t[5];

  trace_reset();
  wo_tickets = 0;
  for(int i = 0; i < 5; i++) // each runs at once and blocks in cond_wait
    t[i] = thread_createv(wo_waiter, 0, "wo", 0, w[i].prio,
                          (void *)(intptr_t)w[i].id);

  mutex_lock(&wo_mutex);
  wo_tickets = 1;
  cond_signal(&wo_cond);
  tr('1');               // 'b' woke, but is now blocked on wo_mutex
  CHECK(count_waiters(&wo_cond) == 4, "signal woke %d of 5",
        5 - count_waiters(&wo_cond));
  mutex_unlock(&wo_mutex);
  tr('2');

  mutex_lock(&wo_mutex);
  wo_tickets = 4;
  cond_broadcast(&wo_cond);
  tr('3');
  mutex_unlock(&wo_mutex);
  tr('4');
  CHECK(LIST_EMPTY(&wo_cond.list), "broadcast left waiters");

  for(int i = 0; i < 5; i++)
    thread_join(t[i]);
  CHECK_TRACE("1b23edac4");
}

// ---- Mutex handover goes to the highest priority waiter, FIFO among
//      equals. Unlocking to an equal priority waiter yields to it, to a
//      lower one does not switch, and then the unlocker may take the
//      mutex back before the waiter runs

static mutex_t mo_mutex = MUTEX_INITIALIZER("mo");

static void *
mo_locker(void *arg)
{
  mutex_lock(&mo_mutex);
  tr((intptr_t)arg);
  mutex_unlock(&mo_mutex);
  tr((intptr_t)arg | 0x20);   // lower case: after the unlock
  return NULL;
}

static void
test_mutex_order(void)
{
  static const struct { char id; uint8_t prio; } w[] = {
    {'A', 3}, {'B', 6}, {'C', 4}, {'D', 6}
  };
  thread_t *t[4];

  trace_reset();
  mutex_lock(&mo_mutex);
  for(int i = 0; i < 4; i++)
    t[i] = thread_createv(mo_locker, 0, "mo", 0, w[i].prio,
                          (void *)(intptr_t)w[i].id);
  tr('m');
  mutex_unlock(&mo_mutex);
  tr('m');
  for(int i = 0; i < 4; i++)
    thread_join(t[i]);
  // 'B' unlocking hands over to 'D' (same prio) at once; 'D' unlocking
  // to 'C' (lower) does not
  CHECK_TRACE("mBDdbCcAam");
}

static void
test_mutex_barging(void)
{
  trace_reset();
  mutex_lock(&mo_mutex);
  thread_t *l = thread_create(mo_locker, (void *)'L', 0, "lo", 0,
                              MAIN_PRIO - 1);
  usleep(100);           // 'L' runs and blocks on the mutex
  tr('a');
  mutex_unlock(&mo_mutex); // 'L' is made ready, but is below us
  tr('b');
  CHECK(mutex_trylock(&mo_mutex) == 0, "trylock of a free mutex failed");
  tr('c');
  usleep(100);           // 'L' runs, finds it taken and goes back to sleep
  CHECK(mutex_trylock(&mo_mutex) != 0, "trylock of a held mutex succeeded");
  tr('d');
  mutex_unlock(&mo_mutex);
  thread_join(l);
  CHECK_TRACE("abcdLl");
}

// ---- Timed waits: exact expiry in virtual time, early wakeup, and the
//      timeout racing a signal at the same instant

static mutex_t tw_mutex = MUTEX_INITIALIZER("tw");
static cond_t tw_cond = COND_INITIALIZER("tw");
static int tw_flag;

static timer_t tw_timer;

static void
tw_timer_cb(void *opaque, uint64_t now)
{
  tw_flag = 1;
  task_wakeup(&tw_cond, 0);
}

static void
tw_arm(uint64_t deadline)
{
  tw_timer.t_cb = tw_timer_cb;
  tw_timer.t_name = "tw";
  const int q = irq_forbid(IRQ_LEVEL_CLOCK);
  timer_arm_abs(&tw_timer, deadline);
  irq_permit(q);
}

static void *
tw_arm_thread(void *arg)
{
  tw_arm((intptr_t)arg);
  return NULL;
}

static void *
tw_signaller(void *arg)
{
  sleep_until((intptr_t)arg);
  mutex_lock(&tw_mutex);
  tw_flag = 1;
  cond_signal(&tw_cond);
  mutex_unlock(&tw_mutex);
  return NULL;
}

static void
test_timed_wait(void)
{
  mutex_lock(&tw_mutex);

  // Nobody signals: exactly the deadline
  uint64_t t0 = clock_get();
  int r = cond_wait_timeout(&tw_cond, &tw_mutex, t0 + 5000);
  uint64_t dt = clock_get() - t0;
  CHECK(r == 1 && dt == 5000, "unsignalled wait: r=%d after %d us", r,
        (int)dt);
  CHECK(mutex_trylock(&tw_mutex) != 0, "mutex not held after timeout");

  // A deadline already passed returns at once
  t0 = clock_get();
  r = cond_wait_timeout(&tw_cond, &tw_mutex, t0 - 1);
  dt = clock_get() - t0;
  CHECK(r == 1 && dt == 0, "past deadline: r=%d after %d us", r, (int)dt);

  // Signalled after 1ms of a 50ms wait. The wait's timer lives on our
  // stack, so if it were not disarmed the sleep past its deadline
  // below would fire it into whatever is there by then.
  tw_flag = 0;
  t0 = clock_get();
  thread_t *t = thread_create(tw_signaller, (void *)(intptr_t)(t0 + 1000),
                              0, "sig", 0, MAIN_PRIO + 1);
  r = 0;
  while(!tw_flag && !r)
    r = cond_wait_timeout(&tw_cond, &tw_mutex, t0 + 50000);
  dt = clock_get() - t0;
  CHECK(r == 0 && tw_flag && dt == 1000, "signalled wait: r=%d flag=%d "
        "after %d us", r, tw_flag, (int)dt);
  mutex_unlock(&tw_mutex);
  thread_join(t);
  usleep(100000);

  // Signal (from a timer callback) and timeout at the same instant.
  // Timers due together fire in an order set by when they were armed,
  // so arming the signal once before and once after the wait's own
  // timer puts the signal first in one of the two runs. Then the wait's
  // timer fires on a thread that is already awake, which must do
  // nothing. Either way the wait reports a timeout: the return value
  // says whether the deadline timer fired, not who woke us.
  for(int after = 0; after < 2; after++) {
    mutex_lock(&tw_mutex);
    tw_flag = 0;
    t0 = clock_get();
    t = NULL;
    if(after) // runs once we are asleep
      t = thread_create(tw_arm_thread, (void *)(intptr_t)(t0 + 2000), 0,
                        "arm", 0, MAIN_PRIO - 1);
    else
      tw_arm(t0 + 2000);
    r = cond_wait_timeout(&tw_cond, &tw_mutex, t0 + 2000);
    dt = clock_get() - t0;
    CHECK(r == 1 && dt == 2000, "racing wait: r=%d after %d us", r, (int)dt);
    mutex_unlock(&tw_mutex);
    if(t)
      thread_join(t);
    CHECK(LIST_EMPTY(&tw_cond.list), "waiter left on the condvar");
    CHECK(tw_flag, "signalling timer never fired");
    usleep(1000);
  }
}

// ---- Raw waitables, woken from a timer callback and from an interrupt

static void
timer_wake_cb(void *opaque, uint64_t now)
{
  task_wakeup(opaque, 0);
}

static task_waitable_t isr_wait = WAITABLE_INITIALIZER("isr");
static volatile int isr_raised;
static int isr_handled;

static void
isr_handler(void *arg)
{
  isr_raised++;
  task_wakeup(&isr_wait, 0);
}

static void *
isr_thread(void *arg)
{
  int q = irq_forbid(IRQ_LEVEL_SCHED);
  while(isr_handled == isr_raised)
    task_sleep_sched_locked(&isr_wait);
  isr_handled++;
  irq_permit(q);
  tr('H');
  return NULL;
}

static int isr_irq = -1;

static void
test_waitable(void)
{
  task_waitable_t w;
  task_waitable_init(&w, "tmr");
  timer_t tm = { .t_cb = timer_wake_cb, .t_opaque = &w, .t_name = "wake" };

  uint64_t t0 = clock_get();
  int q = irq_forbid(IRQ_LEVEL_CLOCK);
  timer_arm_abs(&tm, t0 + 3000);
  irq_permit(q);
  int r = task_sleep_deadline(&w, t0 + 100000);
  uint64_t dt = clock_get() - t0;
  CHECK(r == 0 && dt == 3000, "timer wakeup: r=%d after %d us", r, (int)dt);

  t0 = clock_get();
  r = task_sleep_delta(&w, 2000);
  dt = clock_get() - t0;
  CHECK(r == 1 && dt == 2000, "sleep_delta: r=%d after %d us", r, (int)dt);

  // The handler runs the moment the line is raised and the woken thread
  // preempts us on the way out of it, before host_irq_raise() returns.
  if(isr_irq == -1)
    isr_irq = host_irq_alloc(IRQ_LEVEL_IO, isr_handler, NULL);
  isr_raised = isr_handled = 0;
  trace_reset();
  thread_t *t = thread_create(isr_thread, NULL, 0, "isr", 0, 10);
  tr('a');
  host_irq_raise(isr_irq);
  tr('b');
  thread_join(t);
  CHECK_TRACE("aHb");

  // Raised while the scheduler is locked: handler and switch wait
  trace_reset();
  t = thread_create(isr_thread, NULL, 0, "isr", 0, 10);
  q = irq_forbid(IRQ_LEVEL_SCHED);
  host_irq_raise(isr_irq);
  tr('a');
  irq_permit(q);
  tr('b');
  thread_join(t);
  CHECK_TRACE("aHb");

  // Below the line's level the handler is held off, then runs on permit
  trace_reset();
  t = thread_create(isr_thread, NULL, 0, "isr", 0, 10);
  q = irq_forbid(IRQ_LEVEL_IO);
  const int before = isr_raised;
  host_irq_raise(isr_irq);
  tr(isr_raised != before ? 'X' : 'a');
  irq_permit(q);
  tr('b');
  thread_join(t);
  CHECK_TRACE("aHb");
}

// ---- Lightweight tasks: task_run() is idempotent while queued and
//      priorities apply as for threads

static void lt_fn(task_t *t);
static task_t lt_hi = { .t_run = lt_fn, .t_prio = MAIN_PRIO + 3 };
static task_t lt_lo = { .t_run = lt_fn, .t_prio = MAIN_PRIO - 1 };

static void
lt_fn(task_t *t)
{
  tr(t == &lt_hi ? 'T' : 't');
}

static void
test_lightweight(void)
{
  trace_reset();
  task_run(&lt_hi);
  tr('a');
  int q = irq_forbid(IRQ_LEVEL_SCHED);
  task_run(&lt_hi);
  task_run(&lt_hi);
  tr('b');
  irq_permit(q);
  tr('c');
  task_run(&lt_lo);
  task_run(&lt_lo);
  tr('d');
  usleep(100);
  tr('e');
  task_run(&lt_lo);    // runs again once it has run
  usleep(100);
  CHECK_TRACE("TabTcdtet");
}

// ---- poll() over several condvars

static mutex_t pl_mutex = MUTEX_INITIALIZER("pl");
static cond_t pl_cond[2] = { COND_INITIALIZER("pl0"), COND_INITIALIZER("pl1") };

static void *
pl_signaller(void *arg)
{
  const int which = (intptr_t)arg;
  usleep(1000);
  mutex_lock(&pl_mutex);
  if(which == 0) {
    // We are above the poller, so both fire before it gets to run:
    // the first one is what poll() must return
    cond_signal(&pl_cond[0]);
    cond_signal(&pl_cond[1]);
  } else {
    cond_signal(&pl_cond[1]);
  }
  mutex_unlock(&pl_mutex);
  return NULL;
}

static void
test_poll(void)
{
  const pollset_t ps[3] = {
    { &pl_cond[0], POLL_COND },
    { NULL, POLL_NONE },
    { &pl_cond[1], POLL_COND },
  };

  mutex_lock(&pl_mutex);

  uint64_t t0 = clock_get();
  int r = poll(ps, 3, &pl_mutex, t0 + 7000);
  uint64_t dt = clock_get() - t0;
  CHECK(r == -1 && dt == 7000, "poll timeout: r=%d after %d us", r, (int)dt);
  CHECK(mutex_trylock(&pl_mutex) != 0, "mutex not held after poll");

  for(int which = 0; which < 2; which++) {
    const int prio = which ? MAIN_PRIO - 1 : MAIN_PRIO + 1;
    thread_t *t = thread_create(pl_signaller, (void *)(intptr_t)which, 0,
                                "plsig", 0, prio);
    t0 = clock_get();
    r = poll(ps, 3, &pl_mutex, t0 + 50000);
    dt = clock_get() - t0;
    CHECK(r == which * 2 && dt == 1000, "poll: r=%d after %d us, want %d",
          r, (int)dt, which * 2);
    // Entries for the condvars that did not fire live on our stack and
    // must be gone from the lists
    CHECK(LIST_EMPTY(&pl_cond[0].list) && LIST_EMPTY(&pl_cond[1].list),
          "poll left entries behind");
    mutex_unlock(&pl_mutex);
    thread_join(t);
    mutex_lock(&pl_mutex);
  }
  mutex_unlock(&pl_mutex);
}

// ---- Thread churn: joined and detached threads all go away

static int
count_threads(void)
{
  int n = 0;
  thread_t *t = NULL;
  while((t = thread_get_next(t)) != NULL)
    n++;
  return n;
}

static int churn_ran;

static void *
churn_thread(void *arg)
{
  __atomic_add_fetch(&churn_ran, 1, __ATOMIC_RELAXED);
  if((intptr_t)arg & 1)
    usleep((intptr_t)arg % 50);
  return NULL;
}

#define CHURN 300

static void
test_churn(void)
{
  const int before = count_threads();
  churn_ran = 0;

  for(int i = 0; i < CHURN; i++) {
    thread_t *t = thread_create(churn_thread, (void *)(intptr_t)i, 0,
                                "churn", 0, 1 + i % 6);
    if(t == NULL) {
      CHECK(0, "thread_create failed");
      return;
    }
    thread_join(t);
  }

  for(int i = 0; i < CHURN; i++)
    thread_create(churn_thread, (void *)(intptr_t)i, 0, "churn",
                  TASK_DETACHED, 1 + i % 6);
  usleep(1000);

  CHECK(churn_ran == 2 * CHURN, "%d of %d threads ran", churn_ran,
        2 * CHURN);
  const int after = count_threads();
  CHECK(after == before, "%d threads before churn, %d after", before, after);
}

// ---- Seeded stress

#define FZ_MUTEXES 4
#define FZ_WORKERS 8
#define FZ_OPS     1500

static mutex_t fz_mutex[FZ_MUTEXES];
static thread_t *fz_owner[FZ_MUTEXES];
static int fz_counter[FZ_MUTEXES];
static int fz_expect[FZ_MUTEXES];  // under fz_mutex[i], like the counter

// Tokens: posted under fz_mutex[0], taken by waiters on fz_cond
static cond_t fz_cond = COND_INITIALIZER("fz");
static int fz_tokens;
static int fz_posted;
static int fz_taken;
static int fz_timeouts;

static int fz_irqs;
static int fz_children;
static int fz_stop;
static task_waitable_t fz_irq_wait = WAITABLE_INITIALIZER("fzirq");
static int fz_irq_raised;
static int fz_irq_handled;
static int fz_irq = -1;

static void
fz_isr(void *arg)
{
  fz_irq_raised++;
  task_wakeup(&fz_irq_wait, 0);
}

static void
fz_enter(int i)
{
  mutex_lock(&fz_mutex[i]);
  CHECK(fz_owner[i] == NULL, "mutex %d taken by '%s' while '%s' owns it",
        i, thread_current()->t_name, fz_owner[i]->t_name);
  fz_owner[i] = thread_current();
}

static void
fz_leave(int i)
{
  CHECK(fz_owner[i] == thread_current(), "mutex %d: owner changed", i);
  fz_owner[i] = NULL;
  mutex_unlock(&fz_mutex[i]);
}

// Increment with a window between read and write, so a second thread in
// the critical section shows up as a lost update
static void
fz_bump(int i, int sleep_us)
{
  const int v = fz_counter[i];
  if(sleep_us)
    usleep(sleep_us);
  else if(rand() % 8 == 0)
    host_irq_raise(fz_irq);
  fz_counter[i] = v + 1;
  fz_expect[i]++;
}

static void *
fz_child(void *arg)
{
  const int i = (intptr_t)arg % FZ_MUTEXES;
  fz_enter(i);
  fz_bump(i, (intptr_t)arg & 1);
  fz_leave(i);
  __atomic_add_fetch(&fz_children, 1, __ATOMIC_RELAXED);
  return NULL;
}

// Woken by fz_isr, so it preempts whoever raised the line, quite
// possibly while that thread holds the mutex this one wants
static void *
fz_irq_thread(void *arg)
{
  while(1) {
    int q = irq_forbid(IRQ_LEVEL_SCHED);
    while(fz_irq_handled == fz_irq_raised && !fz_stop)
      task_sleep_sched_locked(&fz_irq_wait);
    if(fz_irq_handled == fz_irq_raised) {
      irq_permit(q);
      return NULL;
    }
    fz_irq_handled++;
    irq_permit(q);

    const int i = rand() % FZ_MUTEXES;
    fz_enter(i);
    fz_bump(i, 0);
    fz_irqs++;
    fz_leave(i);
  }
}

static void
fz_take_token(void)
{
  fz_enter(0);
  const uint64_t deadline = clock_get() + rand() % 200;
  while(fz_tokens == 0) {
    fz_owner[0] = NULL;
    const int r = cond_wait_timeout(&fz_cond, &fz_mutex[0], deadline);
    CHECK(fz_owner[0] == NULL, "cond_wait returned into an owned mutex");
    fz_owner[0] = thread_current();
    if(r) {
      fz_timeouts++;
      break;
    }
  }
  if(fz_tokens) {
    fz_tokens--;
    fz_taken++;
  }
  fz_leave(0);
}

static void
fz_poll_token(void)
{
  // Same thing through poll(), with a second condvar nobody signals
  cond_t idle;
  cond_init(&idle, "fzidle");
  const pollset_t ps[2] = {{&idle, POLL_COND}, {&fz_cond, POLL_COND}};

  fz_enter(0);
  const uint64_t deadline = clock_get() + rand() % 200;
  while(fz_tokens == 0) {
    fz_owner[0] = NULL;
    const int r = poll(ps, 2, &fz_mutex[0], deadline);
    CHECK(fz_owner[0] == NULL, "poll returned into an owned mutex");
    fz_owner[0] = thread_current();
    CHECK(r == -1 || r == 1, "poll returned %d", r);
    if(r == -1) {
      fz_timeouts++;
      break;
    }
  }
  CHECK(LIST_EMPTY(&idle.list), "poll left an entry on its idle condvar");
  if(fz_tokens) {
    fz_tokens--;
    fz_taken++;
  }
  fz_leave(0);
}

static void *
fz_worker(void *arg)
{
  thread_t *kids[4];
  int nkids = 0;

  for(int op = 0; op < FZ_OPS; op++) {
    const int i = rand() % FZ_MUTEXES;
    const int j = rand() % FZ_MUTEXES;

    switch(rand() % 16) {
    case 0 ... 4:  // plain critical section, maybe sleeping in it
      fz_enter(i);
      fz_bump(i, rand() % 4 ? 0 : 1 + rand() % 30);
      fz_leave(i);
      break;

    case 5 ... 6:  // two nested, taken in index order
      if(i != j) {
        const int a = i < j ? i : j;
        const int b = i < j ? j : i;
        fz_enter(a);
        fz_bump(a, 0);
        fz_enter(b);
        fz_bump(b, rand() % 2 ? 0 : 1 + rand() % 10);
        fz_leave(b);
        fz_leave(a);
      }
      break;

    case 7:        // trylock
      if(mutex_trylock(&fz_mutex[i]) == 0) {
        CHECK(fz_owner[i] == NULL, "trylock got an owned mutex %d", i);
        fz_owner[i] = thread_current();
        fz_bump(i, 0);
        fz_leave(i);
      } else {
        CHECK(fz_owner[i] != NULL, "trylock failed on free mutex %d", i);
      }
      break;

    case 8:        // post tokens, fewer than get taken so waits time out
      fz_enter(0);
      const int n = 1 + rand() % 3;
      fz_tokens += n;
      fz_posted += n;
      if(n == 1)
        cond_signal(&fz_cond);
      else
        cond_broadcast(&fz_cond);
      fz_leave(0);
      break;

    case 9 ... 11:
      fz_take_token();
      break;

    case 12:
      fz_poll_token();
      break;

    case 13:       // sleep outside any lock
      usleep(rand() % 50);
      break;

    case 14:       // interrupt outside any lock
      host_irq_raise(fz_irq);
      break;

    case 15:       // a child, joined later or detached
      if(nkids < 4 && rand() % 2) {
        kids[nkids++] = thread_create(fz_child, (void *)(intptr_t)rand(),
                                      0, "fzkid", 0, 1 + rand() % 12);
      } else {
        thread_create(fz_child, (void *)(intptr_t)rand(), 0, "fzkid",
                      TASK_DETACHED, 1 + rand() % 12);
      }
      break;
    }

    if(nkids == 4 || (nkids && op == FZ_OPS - 1)) {
      while(nkids)
        thread_join(kids[--nkids]);
    }
  }
  return NULL;
}

static void
test_stress(void)
{
  for(int i = 0; i < FZ_MUTEXES; i++) {
    mutex_init(&fz_mutex[i], "fz");
    fz_owner[i] = NULL;
    fz_counter[i] = fz_expect[i] = 0;
  }
  if(fz_irq == -1)
    fz_irq = host_irq_alloc(IRQ_LEVEL_IO, fz_isr, NULL);

  const int threads_before = count_threads();
  const uint64_t t0 = clock_get();
  thread_t *irqt = thread_create(fz_irq_thread, NULL, 0, "fzirq", 0, 20);
  thread_t *w[FZ_WORKERS];
  for(int i = 0; i < FZ_WORKERS; i++)
    w[i] = thread_create(fz_worker, NULL, 0, "fzwork", 0, 3 + i % 4);
  for(int i = 0; i < FZ_WORKERS; i++)
    thread_join(w[i]);

  fz_stop = 1;
  task_wakeup(&fz_irq_wait, 0);
  thread_join(irqt);
  usleep(1000);   // let detached children finish and be reaped
  const uint64_t dt = clock_get() - t0;

  for(int i = 0; i < FZ_MUTEXES; i++) {
    CHECK(fz_counter[i] == fz_expect[i], "mutex %d: counter %d, want %d",
          i, fz_counter[i], fz_expect[i]);
    CHECK(fz_owner[i] == NULL, "mutex %d still owned", i);
    CHECK(mutex_trylock(&fz_mutex[i]) == 0, "mutex %d still locked", i);
    mutex_unlock(&fz_mutex[i]);
  }
  CHECK(fz_posted == fz_taken + fz_tokens, "tokens: %d posted, %d taken, "
        "%d left", fz_posted, fz_taken, fz_tokens);
  CHECK(LIST_EMPTY(&fz_cond.list), "waiters left on fz_cond");
  CHECK(fz_irq_handled == fz_irq_raised, "irqs: %d raised, %d handled",
        fz_irq_raised, fz_irq_handled);
  CHECK(count_threads() == threads_before, "threads: %d before, %d after",
        threads_before, count_threads());

  int total = 0;
  for(int i = 0; i < FZ_MUTEXES; i++)
    total += fz_counter[i];
  hosttest_log("kernel: stress %d critical sections, %d tokens, "
               "%d timeouts, %d irqs, %d children, %d.%03d s virtual",
               total, fz_taken, fz_timeouts, fz_irqs, fz_children,
               (int)(dt / 1000000), (int)(dt / 1000 % 1000));
}

// ---- Watchdog: a hang must fail the run, not spin virtual time forever

static void *
watchdog(void *arg)
{
  sleep_until((intptr_t)arg);
  panic("kernel: suite still running after 1000s of virtual time");
}

static int
run_kernel(void)
{
  thread_create(watchdog, (void *)(intptr_t)(clock_get() + 1000000000ull),
                0, "watchdog", TASK_DETACHED, 31);
  check_main_prio();
  test_create();
  test_cond_order();
  test_mutex_order();
  test_mutex_barging();
  test_timed_wait();
  test_waitable();
  test_lightweight();
  test_poll();
  test_churn();
  test_stress();
  return 0; // failures are counted by CHECK()
}

HOSTTEST_SUITE("kernel", run_kernel, 0);
