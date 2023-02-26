#pragma once

#define CLOCK_BASE 0x40000000

#define CLOCK_TASKS_HFCLKSTART    (CLOCK_BASE + 0x000)
#define CLOCK_TASKS_LFCLKSTART    (CLOCK_BASE + 0x008)
#define CLOCK_TASKS_CAL           (CLOCK_BASE + 0x010)

#define CLOCK_EVENTS_HFCLKSTARTED (CLOCK_BASE + 0x100)

#define CLOCK_EVENTS_LFCLKSTARTED (CLOCK_BASE + 0x104)


void nrf52_xtal_enable(void);

void nrf52_lfclk_enable(void);
