#pragma once

#ifdef HAVE_HRTIMER

int hrtimer_arm(timer_t *t, uint64_t expire);

#endif

void systick_timepulse(void);
