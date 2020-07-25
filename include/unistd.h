#pragma once

#include <stdint.h>

void usleep(unsigned int delta);

void sleep(unsigned int seconds);

void sleep_until(uint64_t deadline);

uint64_t clock_get(void);

