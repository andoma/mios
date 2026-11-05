/*
 * "clock-servo": src/util/clock_servo.c against a model of an STM32
 * Ethernet timestamp unit in fine correction mode, the one place the
 * servo's output becomes a clock rate: an addend register, here the
 * STM32N6's, whose nominal value for a 48 MHz HSE (0xfdf7df7d) leaves
 * 0.8 % of headroom below 2^32.
 *
 * A correction beyond that headroom must saturate the addend rather
 * than wrap it: wrapped, the clock ran at under 1 % of its rate, the
 * servo stepped it a second forward every second, and a step left the
 * addend as it was, so the clock never locked again. A PTP master whose
 * time jumps by tens of milliseconds (a main computer booting with its
 * clock off, until NTP steps it) is stepped out instead, on the second
 * Sync that sees it, so a single bad exchange moves nothing.
 *
 * The master is ideal (the servo sees the true offset once a second);
 * what is under test is the servo and the addend, not ptp.c's arithmetic.
 */

#include <stdint.h>

#include <mios/clock_realtime.h>

#include "util/clock_servo.h"

#include "hosttest.h"

#define NSEC 1000000000LL

// The STM32N6's: HSE 48 MHz, ssinc 21 ns (stm32n6_ptp_init)
#define N6_NOMINAL ((uint32_t)((NSEC << 32) / (48000000LL * 21)))

// The board's oscillator against the master's, ppm
#define CRYSTAL_PPM 30

typedef struct sim_clock {
  clock_realtime_t clk;   // first: the class callbacks get this
  int64_t ns;             // the clock's time
  uint32_t nominal;
  uint32_t addend;
  int steps;
} sim_clock_t;


static void
sim_set_time(clock_realtime_t *clk, int64_t nsec)
{
  sim_clock_t *sc = (sim_clock_t *)clk;
  sc->ns = nsec;
  sc->steps++;
}

static int64_t
sim_get_time(clock_realtime_t *clk)
{
  return ((sim_clock_t *)clk)->ns;
}

static void
sim_adj_time(clock_realtime_t *clk, int32_t ppb)
{
  sim_clock_t *sc = (sim_clock_t *)clk;
  sc->addend = clock_realtime_addend(sc->nominal, ppb);
}

static const clock_realtime_class_t sim_clock_class = {
  .name = "sim",
  .set_time = sim_set_time,
  .get_time = sim_get_time,
  .adj_time = sim_adj_time,
};


// The clock after `dt` ns of real time at the current addend
static void
sim_advance(sim_clock_t *sc, int64_t dt)
{
  int64_t d = dt * sc->addend / sc->nominal;
  sc->ns += d + d * CRYSTAL_PPM / 1000000;
}


typedef struct run {
  sim_clock_t sc;
  clock_servo_t cs;
  int64_t master;   // the master's time
} run_t;


static void
run_init(run_t *r)
{
  r->sc = (sim_clock_t) {
    .clk = { .clk_class = &sim_clock_class },
    .nominal = N6_NOMINAL,
    .addend = N6_NOMINAL,
  };
  clock_servo_init(&r->cs, &r->sc.clk);
  r->master = 1793695714 * NSEC;
}

// One second: a servo update on the offset as measured, `error_ns` off
// the truth, then the second passes. Returns the clock's offset from the
// master at its end.
static int64_t
run_second_err(run_t *r, int64_t error_ns)
{
  clock_servo_adjust(&r->cs, r->master - r->sc.ns + error_ns, 1);
  sim_advance(&r->sc, NSEC);
  r->master += NSEC;
  return r->sc.ns - r->master;
}

static int64_t
run_second(run_t *r)
{
  return run_second_err(r, 0);
}

static int64_t
iabs(int64_t v)
{
  return v < 0 ? -v : v;
}


static int
check_addend(void)
{
  int fails = 0;
  const uint32_t n = N6_NOMINAL;

  fails += !CHECK(n == 0xfdf7df7d, "N6 nominal addend 0x%x", n);
  fails += !CHECK(clock_realtime_addend(n, 0) == n, "0 ppb");

  // +1 ppm: up by a millionth
  uint32_t a = clock_realtime_addend(n, 1000);
  fails += !CHECK(a == n + n / 1000000, "+1 ppm gave 0x%x", a);

  // -1.2 %: well inside the register
  a = clock_realtime_addend(n, -12000000);
  fails += !CHECK(a == n - (uint32_t)((int64_t)n * 12 / 1000),
                  "-1.2 %% gave 0x%x", a);

  // +1.2 %: beyond the 0.8 % of headroom, saturated, not wrapped
  a = clock_realtime_addend(n, 12000000);
  fails += !CHECK(a == UINT32_MAX, "+1.2 %% gave 0x%x, want 0x%x", a,
                  UINT32_MAX);

  a = clock_realtime_addend(n, -1000000000);
  fails += !CHECK(a == 0, "-100 %% gave 0x%x", a);
  return fails;
}


// Locked for 30 s from a clock that was never set.
static int
run_locked(run_t *r)
{
  int fails = 0;
  run_init(r);
  for(int i = 0; i < 30; i++)
    run_second(r);
  const int64_t off = r->sc.ns - r->master;
  fails += !CHECK(iabs(off) < 100000, "not locked: %d ns", (int)off);
  fails += !CHECK(r->sc.steps == 1, "%d steps to lock, want the first only",
                  r->sc.steps);
  return fails;
}


// Locked, the master jumps by `jump_us`: the clock must be back within
// 5 ms by `back_s` and stay there for a minute, with `want_steps` steps,
// never further than the jump plus 10 ms on the way.
static int
check_jump(int jump_us, int back_s, int want_steps)
{
  run_t r;
  int fails = run_locked(&r);

  r.master += jump_us * 1000LL;
  const int steps = r.sc.steps;
  int64_t worst = 0;
  int back = -1;
  for(int s = 1; s <= 60; s++) {
    const int64_t off = run_second(&r);
    if(iabs(off) > iabs(worst))
      worst = off;
    if(iabs(off) <= 5000000) {
      if(back < 0)
        back = s;
    } else {
      back = -1;
    }
  }
  hosttest_log("jump %d us: within 5 ms from %d s, worst %d us, %d steps",
               jump_us, back, (int)(worst / 1000), r.sc.steps - steps);

  fails += !CHECK(back > 0 && back <= back_s,
                  "jump %d us: not back within 5 ms by %d s (from %d s)",
                  jump_us, back_s, back);
  fails += !CHECK(iabs(worst) <= iabs(jump_us) * 1000LL + 10000000,
                  "jump %d us: went to %d us on the way", jump_us,
                  (int)(worst / 1000));
  fails += !CHECK(r.sc.steps - steps == want_steps,
                  "jump %d us: %d steps, want %d", jump_us,
                  r.sc.steps - steps, want_steps);
  return fails;
}


// One exchange 30 ms off moves nothing: no step, and the clock stays
// where it was.
static int
check_outlier(void)
{
  run_t r;
  int fails = run_locked(&r);
  const int steps = r.sc.steps;

  int64_t worst = iabs(run_second_err(&r, 30000000));
  for(int s = 0; s < 10; s++) {
    const int64_t off = iabs(run_second(&r));
    if(off > worst)
      worst = off;
  }
  fails += !CHECK(r.sc.steps == steps, "outlier: %d steps",
                  r.sc.steps - steps);
  fails += !CHECK(worst < 100000, "outlier: clock moved %d ns",
                  (int)worst);
  return fails;
}


// A clock that was never set steps on the first offset, and is
// synchronized from there.
static int
check_first_step(void)
{
  int fails = 0;
  run_t r;
  run_init(&r);
  const int64_t off = iabs(run_second(&r));
  fails += !CHECK(r.sc.steps == 1, "first update: %d steps", r.sc.steps);
  fails += !CHECK(r.sc.clk.synchronized, "first update: not synchronized");
  fails += !CHECK(off < 100000, "first update: %d ns off", (int)off);
  return fails;
}


// A step starts the frequency over with the drift estimate.
static int
check_step_restores_frequency(void)
{
  run_t r;
  int fails = run_locked(&r);

  // Slewing out 8 ms takes the addend above nominal, once the median of
  // 8 has moved
  r.master += 8000000;
  for(int i = 0; i < 6; i++)
    run_second(&r);
  fails += !CHECK(r.sc.addend > N6_NOMINAL, "slewing: addend 0x%x",
                  r.sc.addend);

  // A jump beyond the step threshold steps on its second Sync, at the
  // nominal rate after
  r.master += 500 * 1000000LL;
  run_second(&r);
  const int steps = r.sc.steps;
  run_second(&r);
  fails += !CHECK(r.sc.steps == steps + 1, "no step on the second Sync");
  fails += !CHECK(r.sc.addend == N6_NOMINAL,
                  "after a step: addend 0x%x, want 0x%x", r.sc.addend,
                  N6_NOMINAL);
  return fails;
}


static int
run_clock_servo(void)
{
  int fails = 0;
  fails += check_addend();
  fails += check_first_step();
  fails += check_jump(50000, 2, 1);
  fails += check_jump(40000, 2, 1);
  fails += check_jump(-50000, 2, 1);
  fails += check_jump(500000, 2, 1);
  // Below the step threshold: slewed, once the median of 8 has moved
  fails += check_jump(8000, 20, 0);
  fails += check_jump(-8000, 20, 0);
  fails += check_outlier();
  fails += check_step_restores_frequency();
  return fails;
}

HOSTTEST_SUITE("clock-servo", run_clock_servo, 0);
