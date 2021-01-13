#pragma once

#ifdef HAVE_HRTIMER

#include <stdint.h>

struct timer;

int hrtimer_arm(struct timer *t, uint64_t expire);

#endif

#define HZ 100
#define TICKS_PER_US (CPU_SYSTICK_RVR / 1000000)
#define TICKS_PER_HZ (CPU_SYSTICK_RVR / HZ)
