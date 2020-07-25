#pragma once

#include <stdint.h>
#include <sys/queue.h>

typedef struct timer {
  LIST_ENTRY(timer) t_link;
  void (*t_cb)(void *opaque);
  void *t_opaque;
  uint64_t t_expire;
} timer_t;


void timer_arm(timer_t *t, unsigned int delta);

void timer_arm_abs(timer_t *t, uint64_t deadline);

// Return 1 if timer was NOT armed, return 0 if we managed to disarm
int timer_disarm(timer_t *t);
