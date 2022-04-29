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

// IRQ_LEVEL_CLOCK must be blocked
void timer_arm_abs(timer_t *t, uint64_t deadline);

// Return 1 if timer was NOT armed, return 0 if we managed to disarm
// IRQ_LEVEL_CLOCK must be blocked
int timer_disarm(timer_t *t);

void timer_arm_on_queue(timer_t *t, uint64_t deadline, struct timer_list *tl);

void timer_dispatch(struct timer_list *tl, uint64_t now);

void timer_init(timer_t *t, void (*cb)(void *opaque, uint64_t expire),
                void *opaque, const char *name, uint64_t deadline);
