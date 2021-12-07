#pragma once

#include <stdint.h>

void usleep(unsigned int delta);

void usleep_hr(unsigned int useconds);

void sleep(unsigned int seconds);

void sleep_until(uint64_t deadline);

void sleep_until_hr(uint64_t deadline);

uint64_t clock_get(void);

uint64_t clock_get_irq_blocked(void);

void udelay(unsigned int usec); // Busy wait
