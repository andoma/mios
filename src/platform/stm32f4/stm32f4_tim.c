#include <sys/queue.h>
#include <stdint.h>
#include <stdio.h>
#include <unistd.h>
#include <mios/mios.h>
#include <mios/cli.h>

#include "irq.h"
#include "cpu.h"

#include "systick.h"

#include "stm32f4.h"
#include "stm32f4_tim.h"
#include "stm32f4_clk.h"


/************************************************************
 * TIM7 acts as high resolution system timer
 * We offer 1µs precision
 ***********************************************************/

static struct timer_list hr_timers;

// #define HRTIMER_TRACE 16

#ifdef HRTIMER_TRACE

#define HRTIMER_TRACE_FIRE 1
#define HRTIMER_TRACE_ARM  2
#define HRTIMER_TRACE_IRQ  3

typedef struct {
  uint64_t when;
  timer_t *t;
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
  tracebuf[idx].arr  = arr;
  tracebuf[idx].op   = op;
  traceptr++;
}

#endif

static void
hrtimer_rearm(timer_t *t, int64_t now)
{
  const int64_t delta = t->t_expire - now;
  reg_wr(TIM7_BASE + TIMx_CR1, 0x0);
  uint32_t arr;
  if(delta < 2) {
    arr = 2;
  } else if(delta > 65535) {
    arr = 65535;
  } else {
    arr = delta;
  }

#ifdef HRTIMER_TRACE
  hrtimer_trace_add(now, t, arr, HRTIMER_TRACE_ARM);
#endif
  reg_wr(TIM7_BASE + TIMx_CNT, 0xffff);
  reg_wr(TIM7_BASE + TIMx_ARR, arr);
  reg_wr(TIM7_BASE + TIMx_CR1, 0x9);
}


void
irq_55(void)
{
  reg_wr(TIM7_BASE + TIMx_SR, 0x0);

  const int64_t now = clock_get_irq_blocked();
#ifdef HRTIMER_TRACE
  hrtimer_trace_add(now, NULL, 0, HRTIMER_TRACE_IRQ);
#endif

  while(1) {
    timer_t *t = LIST_FIRST(&hr_timers);
    if(t == NULL)
      return;

    int64_t expire = t->t_expire;

    if(expire > now) {
      hrtimer_rearm(t, now);
      return;
    }

#ifdef HRTIMER_TRACE
    hrtimer_trace_add(now, t, 0, HRTIMER_TRACE_FIRE);

    int64_t miss = now - t->t_expire;
    if(miss > 500) {
      panic("Timer %p \"%s\"  missed with %d",
            t, t->t_name, (int)miss);
    }
#endif

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


int
hrtimer_arm(timer_t *t, uint64_t expire)
{
  LIST_INSERT_SORTED(&hr_timers, t, t_link, hrtimer_cmp);
  if(t == LIST_FIRST(&hr_timers))
    hrtimer_rearm(t, clock_get_irq_blocked());
  return 0;
}


static void __attribute__((constructor(1000)))
hrtimer_init(void)
{
  clk_enable(CLK_TIM7);
  irq_enable(55, IRQ_LEVEL_CLOCK);
  reg_wr(TIM7_BASE + TIMx_PSC, STM32F4_TIMERCLOCK / 1000000 - 1);
  reg_wr(TIM7_BASE + TIMx_DIER, 0x1);
  reg_wr(TIM7_BASE + TIMx_CR1, 0x8);
}




static int
cmd_hrt(cli_t *cli, int argc, char **argv)
{
  int64_t now = clock_get_irq_blocked();
  int q = irq_forbid(IRQ_LEVEL_CLOCK);

#ifdef HRTIMER_TRACE
  cli_printf(cli, "%d trace events in total\n", traceptr);

  for(int i = 0; i < HRTIMER_TRACE; i++) {
    int idx = (traceptr + i) & (HRTIMER_TRACE - 1);
    hrtimer_trace_t *ht = &tracebuf[idx];
    switch(ht->op) {
    default:
      continue;
    case HRTIMER_TRACE_FIRE:
      cli_printf(cli, "FIRE\t\t");
      break;
    case HRTIMER_TRACE_ARM:
      cli_printf(cli, "ARM\t%d\t", ht->arr);
      break;
    case HRTIMER_TRACE_IRQ:
      cli_printf(cli, "IRQ\t\t%15d %15d\n",
                 (int)ht->when, (int)(now - ht->when));
      continue;
    }
    cli_printf(cli, "%15d %15d %p\n", (int)ht->when, (int)(now - ht->when),
               ht->t);
  }
#endif

  const timer_t *t;
  LIST_FOREACH(t, &hr_timers, t_link) {
    cli_printf(cli, "%p %p %p %15d %15d %s\n",
               t, t->t_cb, t->t_opaque,
               (int)t->t_expire,
               (int)(t->t_expire - now),
               t->t_name);
  }

  irq_permit(q);
  return 0;
}


CLI_CMD_DEF("hrt", cmd_hrt);


