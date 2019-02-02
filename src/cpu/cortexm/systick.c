#include <stdint.h>
#include <stdio.h>
#include "timer.h"

#include "sys.h"
#include "irq.h"

static volatile unsigned int * const SYST_CSR =   (unsigned int *)0xe000e010;
static volatile unsigned int * const SYST_RVR =   (unsigned int *)0xe000e014;
//static volatile unsigned int * const SYST_CVR =   (unsigned int *)0xe000e018;
//static volatile unsigned int * const SYST_CALIB = (unsigned int *)0xe000e01c;

LIST_HEAD(timer_list, timer);

static struct timer_list timers;

void
exc_systick(void)
{
  struct timer_list pending;
  LIST_INIT(&pending);
  timer_t *t, *n;
  for(t = LIST_FIRST(&timers); t != NULL; t = n) {
    n = LIST_NEXT(t, t_link);
    if(t->t_countdown == 1) {
      LIST_REMOVE(t, t_link);
      LIST_INSERT_HEAD(&pending, t, t_link);
    } else {
      t->t_countdown--;
    }
  }

  while((t = LIST_FIRST(&pending)) != NULL) {
    LIST_REMOVE(t, t_link);
    t->t_countdown = 0;
    t->t_cb(t->t_opaque);
  }
}


void
timer_arm(timer_t *t, unsigned int delta)
{
  if(!delta)
    delta = 1;

  if(t->t_countdown)
    LIST_REMOVE(t, t_link);

  t->t_countdown = delta;
  LIST_INSERT_HEAD(&timers, t, t_link);
}

void
timer_disarm(timer_t *t)
{
  if(!t->t_countdown)
    return;
  LIST_REMOVE(t, t_link);
  t->t_countdown = 0;
}



//static volatile unsigned int * const SYST_SHPR3 = (unsigned int *)0xe000ed20;

void
timer_init(void)
{
  uint32_t timer_calibration = 64000000 / 100;
  *SYST_RVR = timer_calibration - 1;
  *SYST_CSR = 7;
}
