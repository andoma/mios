#pragma once

#ifdef HAVE_HRTIMER

struct timer;

int hrtimer_arm(struct timer *t, uint64_t expire);

#endif

void systick_timepulse(void);

#define HZ 100
#define TICKS_PER_US (SYSTICK_RVR / 1000000)
#define TICKS_PER_HZ (SYSTICK_RVR / HZ)
