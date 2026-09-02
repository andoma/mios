#pragma once

#include <stdint.h>
#include <sys/queue.h>

#include <mios/timer.h>

typedef struct net_task {
  void (*nt_cb)(struct net_task *nt, uint32_t signals);
  STAILQ_ENTRY(net_task) nt_link;
  uint32_t nt_signals;
} net_task_t;

void net_task_raise(net_task_t *nt, uint32_t signals);

// Drop a pending raise, e.g. before freeing the object embedding the task
void net_task_cancel(net_task_t *nt);

void net_timer_arm(timer_t *t, uint64_t deadline);

