/*
 * "clock-servo": src/util/clock_servo.c against a model of an STM32
 * Ethernet timestamp unit in fine correction mode, the one place the
 * servo's output becomes a clock rate: an addend register, here the
 * STM32N6's, whose nominal value for a 48 MHz HSE (0xfdf7df7d) leaves
 * 0.8 % of headroom below 2^32.
 *
 * A PTP master whose time jumps forward by tens of milliseconds (a main
 * computer booting with its clock off, until NTP steps it) makes the
 * servo slew with several percent of correction. That must saturate the
 * addend rather than wrap it: wrapped, the clock ran at under 1 % of its
 * rate, the servo stepped it a second forward every second, and a step
 * left the addend as it was, so the clock never locked again.
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

// One second: a servo update on the offset, then the second passes.
// Returns the clock's offset from the master at its end.
static int64_t
run_second(run_t *r)
{
  clock_servo_adjust(&r->cs, r->master - r->sc.ns, 1);
  sim_advance(&r->sc, NSEC);
  r->master += NSEC;
  return r->sc.ns - r->master;
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


// Locked, the master jumps by `jump_ms`, the clock must come back within
// 5 ms in 45 s and stay, without a step and without getting further than
// the jump plus 10 ms on the way.
static int
check_jump(int jump_ms)
{
  int fails = 0;
  run_t r;
  run_init(&r);

  for(int i = 0; i < 30; i++)
    run_second(&r);
  int64_t off = r.sc.ns - r.master;
  fails += !CHECK(iabs(off) < 100000, "not locked before the jump: %d ns",
                  (int)off);
  fails += !CHECK(r.sc.steps == 1, "%d steps to lock, want the first only",
                  r.sc.steps);

  r.master += jump_ms * 1000000LL;
  const int steps = r.sc.steps;
  int64_t worst = 0;
  int back = -1;
  for(int s = 1; s <= 60; s++) {
    off = run_second(&r);
    if(iabs(off) > iabs(worst))
      worst = off;
    if(iabs(off) <= 5000000) {
      if(back < 0)
        back = s;
    } else {
      back = -1;
    }
  }
  hosttest_log("jump %d ms: within 5 ms from %d s, worst %d us, "
               "addend 0x%x", jump_ms, back, (int)(worst / 1000),
               r.sc.addend);

  fails += !CHECK(back > 0 && back <= 45,
                  "jump %d ms: not back within 5 ms in 45 s (from %d s)",
                  jump_ms, back);
  fails += !CHECK(iabs(worst) <= (iabs(jump_ms) + 10) * 1000000LL,
                  "jump %d ms: went to %d ms on the way", jump_ms,
                  (int)(worst / 1000000));
  fails += !CHECK(r.sc.steps == steps, "jump %d ms: %d steps", jump_ms,
                  r.sc.steps - steps);
  return fails;
}


// A step starts the frequency over with the drift estimate.
static int
check_step_restores_frequency(void)
{
  int fails = 0;
  run_t r;
  run_init(&r);
  run_second(&r);

  // Slewing out 50 ms saturates the addend
  r.master += 50000000;
  for(int i = 0; i < 5; i++)
    run_second(&r);
  fails += !CHECK(r.sc.addend == UINT32_MAX, "slewing: addend 0x%x",
                  r.sc.addend);

  // A jump beyond the step threshold steps, at the nominal rate after
  r.master += 500 * 1000000LL;
  run_second(&r);
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
  fails += check_jump(50);
  fails += check_jump(40);
  fails += check_jump(-50);
  fails += check_step_restores_frequency();
  return fails;
}

HOSTTEST_SUITE("clock-servo", run_clock_servo, 0);
