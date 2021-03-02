#pragma once

#include <stdint.h>
#include <sys/queue.h>

LIST_HEAD(timer_list, timer);

typedef struct timer {
  LIST_ENTRY(timer) t_link;
  void (*t_cb)(void *opaque, uint64_t expire);
  void *t_opaque;
  uint64_t t_expire;
  const char *t_name;
} timer_t;

#define TIMER_HIGHRES 0x1

void timer_arm_abs(timer_t *t, uint64_t deadline, int flags);

// Return 1 if timer was NOT armed, return 0 if we managed to disarm
int timer_disarm(timer_t *t);
