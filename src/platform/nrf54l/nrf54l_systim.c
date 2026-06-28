#include <sys/queue.h>
#include <stdint.h>

#include <mios/mios.h>
#include <mios/timer.h>

#include "irq.h"
#include "nrf54l_reg.h"

// Clock base: the GRTC SYSCOUNTER, a free-running 1 MHz (= microsecond),
// 52-bit counter that is already started by the boot ROM. No setup needed,
// and 52 bits at 1 MHz means no overflow handling for ~139 years.
#define GRTC_BASE            0x500e2000
// SYSCOUNTER read port. Index 0 is free-running and readable from the app
// core; the per-domain port (index GRTC_IRQ_GROUP) stays parked unless its
// ACTIVE/KEEPRUNNING is requested, so it reads frozen. Index 0 is fine as
// long as we don't put the GRTC into a low-power/parked state.
#define GRTC_SYS_IDX         0
#define GRTC_SYSCOUNTERL    (GRTC_BASE + 0x720 + GRTC_SYS_IDX * 0x10)
#define GRTC_SYSCOUNTERH    (GRTC_BASE + 0x724 + GRTC_SYS_IDX * 0x10)
#define GRTC_SYS_VALUE       0x000fffff  // SYSCOUNTERH low 20 bits = counter[51:32]
#define GRTC_SYS_OVERFLOW    (1u << 31)  // low 32 bits wrapped during the read

#define GRTC_TASKS_START     (GRTC_BASE + 0x060)
#define GRTC_MODE            (GRTC_BASE + 0x510)
#define GRTC_MODE_SYSCOUNTEREN (1u << 1) // AUTOEN=0 keeps the counter active

// One-shot timer for arming the next deadline: TIMER20, run at 1 MHz /
// 32-bit to match the microsecond clock base. Note the nRF54L TIMER is
// clocked from PCLK1M (1 MHz), so PRESCALER=0 gives 1 MHz (unlike nRF52,
// whose TIMER is clocked from 16 MHz and needs PRESCALER=4).
#define TIMER_BASE           0x500ca000  // TIMER20
#define TIMER_IRQ            202         // TIMER20
#define TIMER_TASKS_START    0x000
#define TIMER_TASKS_STOP     0x004
#define TIMER_TASKS_CLEAR    0x00c
#define TIMER_EVENTS_COMPARE 0x140
#define TIMER_INTENSET       0x304
#define TIMER_MODE           0x504
#define TIMER_BITMODE        0x508
#define TIMER_PRESCALER      0x510
#define TIMER_CC0            0x540

#define TIMER_MAX            0xffffff  // re-arm horizon (~16.7 s @ 1 MHz)

static struct timer_list systim_timers;

// The GRTC free-runs from power-on and is not reset when we boot, so we
// subtract its value at first read to make clock_get() boot-relative.
static uint64_t clock_base;
static uint8_t clock_base_set;


static uint64_t
grtc_read(void)
{
  // Reading SYSCOUNTERL latches the high word; SYSCOUNTERH.OVERFLOW tells us
  // the low part wrapped during the read, in which case we read again. Do
  // NOT spin on BUSY: at boot it can be set indefinitely and would hang.
  for(int i = 0; i < 8; i++) {
    uint32_t l = reg_rd(GRTC_SYSCOUNTERL);
    uint32_t h = reg_rd(GRTC_SYSCOUNTERH);
    if(h & GRTC_SYS_OVERFLOW)
      continue;
    return ((uint64_t)(h & GRTC_SYS_VALUE) << 32) | l;
  }
  uint32_t l = reg_rd(GRTC_SYSCOUNTERL);
  uint32_t h = reg_rd(GRTC_SYSCOUNTERH);
  return ((uint64_t)(h & GRTC_SYS_VALUE) << 32) | l;
}


int64_t
clock_get_irq_blocked(void)
{
  uint64_t v = grtc_read();
  if(!clock_base_set) {
    clock_base = v;
    clock_base_set = 1;
  }
  return v - clock_base;
}


uint64_t
clock_get(void)
{
  int s = irq_forbid(IRQ_LEVEL_CLOCK);
  uint64_t r = clock_get_irq_blocked();
  irq_permit(s);
  return r;
}


void
udelay(unsigned int usec)
{
  int s = irq_forbid(IRQ_LEVEL_CLOCK);
  uint64_t deadline = clock_get_irq_blocked() + usec;
  while(clock_get_irq_blocked() < deadline) {}
  irq_permit(s);
}


// SysTick is unused on this platform (the GRTC is the time base), but the
// vector table still references this symbol.
void
exc_systick(void)
{
}


// Program the one-shot to fire at the head of the timer list. Caller must
// hold IRQ_LEVEL_CLOCK.
static void
systim_rearm(int64_t now)
{
  reg_wr(TIMER_BASE + TIMER_TASKS_STOP, 1);

  const timer_t *t = LIST_FIRST(&systim_timers);
  if(t == NULL)
    return;

  int64_t delta = t->t_expire - now;
  uint32_t d;
  if(delta < 2)
    d = 2;
  else if(delta > TIMER_MAX)
    d = TIMER_MAX; // wake up early and re-arm for distant deadlines
  else
    d = delta;

  reg_wr(TIMER_BASE + TIMER_TASKS_CLEAR, 1);
  reg_wr(TIMER_BASE + TIMER_CC0, d);
  reg_wr(TIMER_BASE + TIMER_EVENTS_COMPARE, 0);
  reg_wr(TIMER_BASE + TIMER_TASKS_START, 1);
}


// TIMER20 compare interrupt
void
irq_202(void)
{
  if(!reg_rd(TIMER_BASE + TIMER_EVENTS_COMPARE))
    return;
  reg_wr(TIMER_BASE + TIMER_EVENTS_COMPARE, 0);

  const int64_t now = clock_get_irq_blocked();

  while(1) {
    timer_t *t = LIST_FIRST(&systim_timers);
    if(t == NULL)
      break;

    if(t->t_expire > now) {
      systim_rearm(now);
      break;
    }

    uint64_t expire = t->t_expire;
    LIST_REMOVE(t, t_link);
    t->t_expire = 0;
    t->t_cb(t->t_opaque, expire);
  }
}


static int
systim_cmp(const timer_t *a, const timer_t *b)
{
  return a->t_expire > b->t_expire;
}


// IRQ_LEVEL_CLOCK must be blocked by the caller
void
timer_arm_abs(timer_t *t, uint64_t deadline)
{
  timer_disarm(t);

  t->t_expire = deadline;
  LIST_INSERT_SORTED(&systim_timers, t, t_link, systim_cmp);
  if(t == LIST_FIRST(&systim_timers))
    systim_rearm(clock_get_irq_blocked());
}


static void __attribute__((constructor(130)))
nrf54l_systim_init(void)
{
  reg_wr(TIMER_BASE + TIMER_MODE, 0);      // timer mode
  reg_wr(TIMER_BASE + TIMER_BITMODE, 3);   // 32-bit
  reg_wr(TIMER_BASE + TIMER_PRESCALER, 0); // PCLK1M / 2^0 = 1 MHz
  reg_wr(TIMER_BASE + TIMER_INTENSET, 1 << 16); // COMPARE0
  irq_enable(TIMER_IRQ, IRQ_LEVEL_CLOCK);

  // Start the GRTC SYSCOUNTER. The boot ROM normally starts it, but after an
  // ERASEALL (or on a cold part) it is stopped, so do it ourselves. AUTOEN=0
  // keeps the counter active; SYSCOUNTEREN enables it; TASKS_START requests
  // the LFCLK and starts counting.
  reg_wr(GRTC_MODE, GRTC_MODE_SYSCOUNTEREN);
  reg_wr(GRTC_TASKS_START, 1);

  clock_get_irq_blocked(); // capture the boot-time GRTC offset
}
