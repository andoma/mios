#include <sys/queue.h>
#include <stdint.h>
#include <stdio.h>
#include <unistd.h>
#include "irq.h"
#include "cpu.h"

#include "clk_config.h"
#include "systick.h"

#include "stm32f4.h"
#include "stm32f4_tim.h"
#include "stm32f4_clk.h"


/************************************************************
 * TIM7 acts as high resolution system timer
 * We offer 10us precision
 ***********************************************************/

static struct timer_list hr_timers;

static void
hrtimer_rearm(timer_t *t, int64_t now)
{
  const int64_t delta = t->t_expire - now;

  reg_wr(TIM7_BASE + TIMx_CR1, 0x8);
  uint32_t arr;
  if(delta < 1) {
    arr = 1;
  } else if(delta > 655350) {
    arr = 655350;
  } else {
    arr = delta;
  }

  reg_wr(TIM7_BASE + TIMx_ARR, ((9 + arr) / 10) - 1);
  reg_wr(TIM7_BASE + TIMx_CR1, 0x9);
}


void
irq_55(void)
{
  reg_wr(TIM7_BASE + TIMx_SR, 0x0);
  const int64_t now = clock_get_irq_blocked();

  while(1) {
    timer_t *t = LIST_FIRST(&hr_timers);
    if(t == NULL)
      return;

    if(t->t_expire > now) {
      hrtimer_rearm(t, now);
      return;
    }
    LIST_REMOVE(t, t_link);
    t->t_expire = 0;
    t->t_cb(t->t_opaque);
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
  // Configure prescaler for 10us precision
  reg_wr(TIM7_BASE + TIMx_PSC, TIMERCLOCK / 100000 - 1);
  reg_wr(TIM7_BASE + TIMx_DIER, 0x1);
  reg_wr(TIM7_BASE + TIMx_CR1, 0x8);
}


#if 0

#include "cli.h"


static int
cmd_sleep(cli_t *cli, int argc, char **argv)
{
  int64_t p = clock_get();
  int64_t t = p + 323456;
  sleep_until_hr(t);
  int64_t a = clock_get();

  printf("Slept for %dus   TARGET:%d\n", (int)(a - p), (int)t);
  return 0;
}


CLI_CMD_DEF("sleep", cmd_sleep);



#include <io.h>

static int
cmd_pulsar(cli_t *cli, int argc, char **argv)
{

  int64_t c = clock_get();

  gpio_t pin = GPIO_PA(6);
  gpio_conf_output(pin, GPIO_PUSH_PULL,
                   GPIO_SPEED_HIGH, GPIO_PULL_NONE);

  while(1) {
    c += 250;
    while(clock_get() < c) {
    }
    gpio_set_output(pin, 1);
    c += 250;
    while(clock_get() < c) {
    }
    gpio_set_output(pin, 0);
  }
  return 0;
}


CLI_CMD_DEF("pulsar", cmd_pulsar);


static gpio_t mt_pin;
int mt_pin_val;
static timer_t mt[50];

static void
mt_cb(void *x)
{
  mt_pin_val = !mt_pin_val;
  gpio_set_output(mt_pin, mt_pin_val);
}


static int
cmd_mt(cli_t *cli, int argc, char **argv)
{
  int64_t ts = clock_get() + 100000;

  mt_pin = GPIO_PA(6);
  gpio_conf_output(mt_pin, GPIO_PUSH_PULL,
                   GPIO_SPEED_HIGH, GPIO_PULL_NONE);

  mt_pin_val = !mt_pin_val;
  gpio_set_output(mt_pin, mt_pin_val);

  int q = irq_forbid(IRQ_LEVEL_CLOCK);
  for(int i = 0; i < 50; i++) {
    mt[i].t_cb = mt_cb;
    timer_arm_abs(&mt[i], ts - i * 100, TIMER_HIGHRES);
  }
  irq_permit(q);

  return 0;
}


CLI_CMD_DEF("mt", cmd_mt);
#endif
