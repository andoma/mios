#pragma once

#include <stdint.h>

// Bump the bucket covering `pc`. Called from the platform's sampling IRQ
// handler at IRQ_LEVEL_PROFILE.
void profile_sample(uint32_t pc);
