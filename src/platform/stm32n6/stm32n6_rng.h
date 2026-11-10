#pragma once

#include <stdint.h>

// How rand() has fared with the hardware generator, see stm32n6_rng.c
typedef struct {
  uint32_t ok;        // Values taken from it
  uint32_t timeouts;  // It had nothing ready in time
  uint32_t errors;    // It reported a seed or clock error
} stm32n6_rng_stats_t;

extern stm32n6_rng_stats_t stm32n6_rng_stats;
