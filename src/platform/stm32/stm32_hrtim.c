#include <assert.h>
#include <sys/queue.h>
#include <stdint.h>
#include <stdio.h>
#include <unistd.h>
#include <mios/mios.h>
#include <mios/cli.h>

#include "irq.h"
#include "cpu.h"

#include "systick.h"

static uint32_t g_hrtim_regbase;

static struct timer_list hr_timers;

// #define HRTIMER_TRACE 16

#ifdef HRTIMER_TRACE

static volatile unsigned int * const SYST_VAL = (unsigned int *)0xe000e018;

#define HRTIMER_TRACE_FIRE  1
#define HRTIMER_TRACE_ARM   2
#define HRTIMER_TRACE_IRQ   3

typedef struct {
  uint64_t when;
  timer_t *t;
  uint32_t syst_val;
  uint16_t arr;
  uint8_t op;
} hrtimer_trace_t;

static int traceptr;
static hrtimer_trace_t tracebuf[HRTIMER_TRACE];

static void
hrtimer_trace_add(uint64_t when, timer_t *t,
                  uint16_t arr, uint8_t op)
{
  int idx = traceptr & (HRTIMER_TRACE - 1);
  tracebuf[idx].when = when;
  tracebuf[idx].t    = t;
  tracebuf[idx].syst_val = *SYST_VAL;
  tracebuf[idx].arr  = arr;
  tracebuf[idx].op   = op;
  traceptr++;
}


static void
hrtimer_dump_trace(stream_t *s)
{
  int64_t now = clock_get_irq_blocked();

  stprintf(s, "%d trace events in total\n", traceptr);

  for(int i = 0; i < HRTIMER_TRACE; i++) {
    int idx = (traceptr + i) & (HRTIMER_TRACE - 1);
    hrtimer_trace_t *ht = &tracebuf[idx];
    stprintf(s, "%08x\t", ht->syst_val);
    switch(ht->op) {
    default:
      continue;
    case HRTIMER_TRACE_FIRE:
      stprintf(s, "FIRE\t\t");
      break;
    case HRTIMER_TRACE_ARM:
      stprintf(s, "ARM\t%d\t", ht->arr);
      break;
    case HRTIMER_TRACE_IRQ:
      stprintf(s, "IRQ\t\t%15d %15d\n",
                 (int)ht->when, (int)(now - ht->when));
      continue;
    }
    stprintf(s, "%15d %15d %p\n", (int)ht->when, (int)(now - ht->when),
               ht->t);
  }

  const timer_t *t;
  LIST_FOREACH(t, &hr_timers, t_link) {
    stprintf(s, "%p %p %p %15d %15d %s\n",
               t, t->t_cb, t->t_opaque,
               (int)t->t_expire,
               (int)(t->t_expire - now),
               t->t_name);
  }
}







#endif

static void
hrtimer_rearm(timer_t *t, int64_t now)
{
  const int64_t delta = t->t_expire - now;
  const uint32_t regbase = g_hrtim_regbase;
  reg_wr(regbase + TIMx_CR1, 0x0);
  uint32_t arr;
  if(delta < 2) {
    arr = 2;
  } else if(delta > 65534) {
    arr = 65534;
  } else {
    arr = delta;
  }

#ifdef HRTIMER_TRACE
  hrtimer_trace_add(now, t, arr, HRTIMER_TRACE_ARM);
#endif
  reg_wr(regbase + TIMx_CNT, 0xffff);
  reg_wr(regbase + TIMx_ARR, arr);
  reg_wr(regbase + TIMx_CR1, 0x9);
}


static void
hrtimer_irq(void *arg)
{
  const uint32_t regbase = g_hrtim_regbase;
  reg_wr(regbase + TIMx_SR, 0x0);

  const int64_t now = clock_get_irq_blocked();
#ifdef HRTIMER_TRACE
  hrtimer_trace_add(now, NULL, 0, HRTIMER_TRACE_IRQ);
#endif

  while(1) {
    timer_t *t = LIST_FIRST(&hr_timers);
    if(t == NULL)
      break;

    if(t->t_expire > now) {
      hrtimer_rearm(t, now);
      break;
    }

#ifdef HRTIMER_TRACE
    hrtimer_trace_add(now, t, 0, HRTIMER_TRACE_FIRE);

    int64_t miss = now - t->t_expire;
    if(miss > 500) {
      hrtimer_dump_trace(stdio);
      panic("Timer %p \"%s\"  missed with %d",
            t, t->t_name, (int)miss);
    }
#endif

    uint64_t expire = t->t_expire;
    LIST_REMOVE(t, t_link);
    t->t_expire = 0;
    t->t_cb(t->t_opaque, expire);
  }
}


static int
hrtimer_cmp(const timer_t *a, const timer_t *b)
{
  return a->t_expire > b->t_expire;
}


void
timer_arm_abs(timer_t *t, uint64_t deadline)
{
  timer_disarm(t);

  t->t_expire = deadline;
  LIST_INSERT_SORTED(&hr_timers, t, t_link, hrtimer_cmp);
  if(t == LIST_FIRST(&hr_timers))
    hrtimer_rearm(t, clock_get_irq_blocked());
}


static error_t
hrtimer_init(uint32_t regbase, uint16_t clkid, int irq)
{
  clk_enable(clkid);
  if(!clk_is_enabled(clkid))
    return ERR_NO_DEVICE;
  g_hrtim_regbase = regbase;
  reg_wr(regbase + TIMx_PSC, clk_get_freq(clkid) / 1000000 - 1);
  reg_wr(regbase + TIMx_DIER, 0x1);
  reg_wr(regbase + TIMx_CR1, 0x0);
  irq_enable_fn_arg(irq, IRQ_LEVEL_CLOCK, hrtimer_irq, NULL);
  return 0;
}

#ifdef HRTIMER_TRACE

static error_t
cmd_hrt(cli_t *cli, int argc, char **argv)
{
  int q = irq_forbid(IRQ_LEVEL_CLOCK);
  hrtimer_dump_trace(cli->cl_stream);
  irq_permit(q);
  return 0;
}


CLI_CMD_DEF("hrt", cmd_hrt);


#endif
