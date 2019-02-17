#include <stdio.h>
#include <stdint.h>

#include "timer.h"


LIST_HEAD(timer_list, timer);

static struct timer_list timers;


#define CLINT_BASE  0x2000000 // Should probably be in platform.h


volatile static uint64_t* mtime =      (uint64_t*)(CLINT_BASE + 0xbff8);
volatile static uint64_t* timecmp =    (uint64_t*)(CLINT_BASE + 0x4000);

void
timer_init(void)
{
  __asm volatile("csrs mie,%0"::"r"(0x80)); // Enable timer interrupt
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




void
timer_interrupt(void)
{
  uint64_t period = 0xc0000;

  if(*timecmp == 0) {
    *timecmp = *mtime + period;
  } else {
    *timecmp += period;
  }

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
