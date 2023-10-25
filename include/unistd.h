#pragma once

#include <stdint.h>

void usleep(unsigned int delta);

void sleep(unsigned int seconds);

void sleep_until(uint64_t deadline);

uint64_t clock_get(void);

uint64_t clock_get_irq_blocked(void);

void udelay(unsigned int usec); // Busy wait
