/*
 * Tests for the host DSIG bus (host/dsig/dsig.c).
 *
 * The cases here pin down what dsig_unsub() promises a caller that is
 * about to free the callback's opaque: once it returns, the callback is
 * not running and will not run again. dsig_input() and the TTL expiry in
 * bus_thread() copy (cb, opaque) under the bus lock and call them after
 * unlocking, so without that guarantee a frame that is being delivered
 * while another thread unsubscribes calls into freed memory. That is how
 * tissuescanner crashed: two subscribers on one signal, the first
 * callback slow, the second unsubscribed and freed while it waited its
 * turn.
 *
 * Every case is driven by explicit handshakes, not by timing luck, so a
 * failure reproduces on every run.
 */
#include "dsig.h"

#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define SIG_A 0x601

// How long a case waits for something that should not happen before
// deciding it does not. Long enough to cover scheduling noise.
#define SETTLE_US 100000

static int failures;
static const char *current_case;

static void
failf(int line, const char *fmt, ...)
{
  va_list ap;
  va_start(ap, fmt);
  fprintf(stderr, "  FAIL %s:%d: ", current_case, line);
  vfprintf(stderr, fmt, ap);
  fprintf(stderr, "\n");
  va_end(ap);
  failures++;
}

#define CHECK(cond, ...) do { if(!(cond)) failf(__LINE__, __VA_ARGS__); } while(0)

static void
sleep_us(int64_t us)
{
  struct timespec ts = { .tv_sec = us / 1000000,
                         .tv_nsec = (us % 1000000) * 1000 };
  nanosleep(&ts, NULL);
}

// Wait for *flag to become nonzero. Returns 0 on timeout.
static int
wait_flag(atomic_int *flag, int64_t timeout_us)
{
  for(int64_t t = 0; t < timeout_us; t += 1000) {
    if(atomic_load(flag))
      return 1;
    sleep_us(1000);
  }
  return atomic_load(flag);
}

static void
tx_nop(void *opaque, uint32_t signal, const void *data, size_t len)
{
}


/*
 * A callback that announces it is running, then blocks until released.
 */
typedef struct {
  atomic_int entered;
  atomic_int release;
  atomic_int done;
  int want_null;              // only block on a TTL expiry (data == NULL)
} blocker_t;

static void
blocker_cb(void *opaque, uint32_t signal, const void *data, size_t len)
{
  blocker_t *b = opaque;
  if(b->want_null && data != NULL)
    return;
  atomic_store(&b->entered, 1);
  wait_flag(&b->release, 5000000);
  atomic_store(&b->done, 1);
}


/*
 * Counts calls, and remembers whether any came after its owner was told
 * the subscription was gone.
 */
typedef struct {
  atomic_int calls;
  atomic_int late_calls;
  atomic_int unsub_returned;
} counter_t;

static void
counter_cb(void *opaque, uint32_t signal, const void *data, size_t len)
{
  counter_t *c = opaque;
  atomic_fetch_add(&c->calls, 1);
  if(atomic_load(&c->unsub_returned))
    atomic_fetch_add(&c->late_calls, 1);
}


typedef struct {
  dsig_t *bus;
  uint32_t signal;
} input_arg_t;

static void *
input_thread(void *arg)
{
  input_arg_t *a = arg;
  uint32_t v = 428;
  dsig_input(a->bus, a->signal, &v, sizeof(v));
  return NULL;
}


typedef struct {
  dsig_sub_t *sub;
  atomic_int returned;
  // Snapshot of the callback's state at the moment dsig_unsub() returned
  atomic_int *observe;
  int observed;
  atomic_int *notify;
} unsub_arg_t;

static void *
unsub_thread(void *arg)
{
  unsub_arg_t *a = arg;
  dsig_unsub(a->sub);
  if(a->observe != NULL)
    a->observed = atomic_load(a->observe);
  if(a->notify != NULL)
    atomic_store(a->notify, 1);
  atomic_store(&a->returned, 1);
  return NULL;
}


/*
 * dsig_unsub() of a subscription whose callback is running, called from
 * another thread, must not return until the callback has returned.
 */
static void
test_unsub_waits_for_running_callback(void)
{
  dsig_t *bus = dsig_create(tx_nop, NULL);
  blocker_t b = {};
  dsig_sub_t *s = dsig_sub(bus, SIG_A, 0xffffffff, 0, blocker_cb, &b);

  pthread_t in;
  input_arg_t ia = { bus, SIG_A };
  pthread_create(&in, NULL, input_thread, &ia);
  CHECK(wait_flag(&b.entered, 2000000), "callback never ran");

  pthread_t un;
  unsub_arg_t ua = { .sub = s, .observe = &b.done };
  pthread_create(&un, NULL, unsub_thread, &ua);

  sleep_us(SETTLE_US);
  CHECK(!atomic_load(&ua.returned),
        "dsig_unsub() returned while the callback was still running");

  atomic_store(&b.release, 1);
  pthread_join(in, NULL);
  pthread_join(un, NULL);
  CHECK(ua.observed, "callback had not finished when dsig_unsub() returned");

  dsig_destroy(bus);
}


/*
 * The tissuescanner crash. Two subscribers on one signal: dsig_input()
 * snapshots both, then blocks in the first. Meanwhile the second is
 * unsubscribed. Once that dsig_unsub() has returned, the second callback
 * must never be called, because its owner is free to destroy the opaque.
 */
static void
test_unsubbed_callback_not_called_after_unsub(void)
{
  dsig_t *bus = dsig_create(tx_nop, NULL);
  blocker_t b = {};
  counter_t c = {};
  dsig_sub(bus, SIG_A, 0xffffffff, 0, blocker_cb, &b);
  dsig_sub_t *sc = dsig_sub(bus, SIG_A, 0xffffffff, 0, counter_cb, &c);

  pthread_t in;
  input_arg_t ia = { bus, SIG_A };
  pthread_create(&in, NULL, input_thread, &ia);
  CHECK(wait_flag(&b.entered, 2000000), "first callback never ran");

  pthread_t un;
  unsub_arg_t ua = { .sub = sc, .notify = &c.unsub_returned };
  pthread_create(&un, NULL, unsub_thread, &ua);

  // Give a broken dsig_unsub() time to return before the first callback
  // lets dsig_input() move on to the second.
  sleep_us(SETTLE_US);
  atomic_store(&b.release, 1);
  pthread_join(in, NULL);
  pthread_join(un, NULL);

  CHECK(atomic_load(&c.late_calls) == 0,
        "callback called %d time(s) after dsig_unsub() returned",
        atomic_load(&c.late_calls));

  dsig_destroy(bus);
}


/*
 * Same guarantee for the TTL expiry path, which bus_thread() fires with
 * data == NULL after dropping the lock.
 */
static void
test_unsub_waits_for_running_expiry(void)
{
  dsig_t *bus = dsig_create(tx_nop, NULL);
  blocker_t b = { .want_null = 1 };
  dsig_sub_t *s = dsig_sub(bus, SIG_A, 0xffffffff, 20, blocker_cb, &b);

  CHECK(wait_flag(&b.entered, 2000000), "expiry callback never ran");

  pthread_t un;
  unsub_arg_t ua = { .sub = s, .observe = &b.done };
  pthread_create(&un, NULL, unsub_thread, &ua);

  sleep_us(SETTLE_US);
  CHECK(!atomic_load(&ua.returned),
        "dsig_unsub() returned while the expiry callback was still running");

  atomic_store(&b.release, 1);
  pthread_join(un, NULL);
  CHECK(ua.observed,
        "expiry callback had not finished when dsig_unsub() returned");

  dsig_destroy(bus);
}


/*
 * A callback may unsubscribe itself. That must neither deadlock (it is
 * the callback dsig_unsub() would wait for) nor leave the callback live.
 */
typedef struct {
  dsig_sub_t *self;
  atomic_int calls;
} self_unsub_t;

static void
self_unsub_cb(void *opaque, uint32_t signal, const void *data, size_t len)
{
  self_unsub_t *u = opaque;
  atomic_fetch_add(&u->calls, 1);
  dsig_unsub(u->self);
}

static void
test_unsub_from_own_callback(void)
{
  dsig_t *bus = dsig_create(tx_nop, NULL);
  self_unsub_t u = {};
  u.self = dsig_sub(bus, SIG_A, 0xffffffff, 0, self_unsub_cb, &u);

  uint32_t v = 1;
  dsig_input(bus, SIG_A, &v, sizeof(v));
  dsig_input(bus, SIG_A, &v, sizeof(v));
  CHECK(atomic_load(&u.calls) == 1, "callback ran %d times, expected 1",
        atomic_load(&u.calls));

  dsig_destroy(bus);
}


/*
 * The crash as it happens in the field, without handshakes: frames keep
 * arriving while another thread subscribes, unsubscribes and frees the
 * opaque straight away, the way util::dsig::Subscriber's destructor
 * does. A slow subscriber on the same signal stretches the window. The
 * check is that no callback ever sees a freed opaque; under 'make asan'
 * a use-after-free is also reported directly.
 */
#define STRESS_MAGIC 0x5ca1ab1e

typedef struct {
  atomic_int magic;
} stress_opaque_t;

static atomic_int stress_bad;
static atomic_int stress_stop;

static void
stress_slow_cb(void *opaque, uint32_t signal, const void *data, size_t len)
{
  sleep_us(50);
}

static void
stress_cb(void *opaque, uint32_t signal, const void *data, size_t len)
{
  stress_opaque_t *o = opaque;
  if(atomic_load(&o->magic) != STRESS_MAGIC)
    atomic_fetch_add(&stress_bad, 1);
}

static void *
stress_input_thread(void *arg)
{
  dsig_t *bus = arg;
  uint32_t v = 428;
  while(!atomic_load(&stress_stop))
    dsig_input(bus, SIG_A, &v, sizeof(v));
  return NULL;
}

static void
test_unsub_and_free_under_traffic(void)
{
  dsig_t *bus = dsig_create(tx_nop, NULL);
  atomic_store(&stress_bad, 0);
  atomic_store(&stress_stop, 0);
  dsig_sub_t *slow = dsig_sub(bus, SIG_A, 0xffffffff, 0,
                              stress_slow_cb, NULL);

  pthread_t in[2];
  for(int i = 0; i < 2; i++)
    pthread_create(&in[i], NULL, stress_input_thread, bus);

  for(int i = 0; i < 20000; i++) {
    stress_opaque_t *o = malloc(sizeof(*o));
    atomic_store(&o->magic, STRESS_MAGIC);
    dsig_sub_t *s = dsig_sub(bus, SIG_A, 0xffffffff, 0, stress_cb, o);
    if(i & 1)
      sleep_us(10);
    dsig_unsub(s);
    // Scribble before freeing, so a late call is caught without ASan too
    atomic_store(&o->magic, 0);
    free(o);
  }

  atomic_store(&stress_stop, 1);
  for(int i = 0; i < 2; i++)
    pthread_join(in[i], NULL);
  dsig_unsub(slow);

  CHECK(atomic_load(&stress_bad) == 0,
        "%d callback(s) ran on an opaque already freed after dsig_unsub()",
        atomic_load(&stress_bad));

  dsig_destroy(bus);
}


static void
on_alarm(int sig)
{
  static const char msg[] = "FAIL: timed out, probably deadlocked\n";
  ssize_t r = write(2, msg, sizeof(msg) - 1);
  (void)r;
  _exit(1);
}

typedef struct {
  const char *name;
  void (*fn)(void);
} testcase_t;

static const testcase_t cases[] = {
  { "unsub_waits_for_running_callback",
    test_unsub_waits_for_running_callback },
  { "unsubbed_callback_not_called_after_unsub",
    test_unsubbed_callback_not_called_after_unsub },
  { "unsub_waits_for_running_expiry",
    test_unsub_waits_for_running_expiry },
  { "unsub_from_own_callback",
    test_unsub_from_own_callback },
  { "unsub_and_free_under_traffic",
    test_unsub_and_free_under_traffic },
};

int
main(int argc, char **argv)
{
  signal(SIGALRM, on_alarm);

  int failed_cases = 0;
  for(size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
    if(argc > 1 && strcmp(argv[1], cases[i].name))
      continue;
    current_case = cases[i].name;
    int before = failures;
    alarm(10);
    cases[i].fn();
    alarm(0);
    int ok = failures == before;
    printf("%s %s\n", ok ? "PASS" : "FAIL", cases[i].name);
    failed_cases += !ok;
  }
  printf("%d failed\n", failed_cases);
  return failed_cases ? 1 : 0;
}
