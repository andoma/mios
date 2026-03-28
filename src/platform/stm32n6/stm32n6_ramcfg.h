#pragma once

#include "stm32n6_reg.h"

#define RAMCFG_BASE 0x52023000

#define RAMCFG_AXISRAMxCR(x) (RAMCFG_BASE + 0x80 + 0x80 * ((x) - 2))
