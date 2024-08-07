#pragma once

#include "stm32g4_reg.h"

#define PWR_BASE 0x40007000

#define PWR_CR1  (PWR_BASE + 0x00)
#define PWR_CR5  (PWR_BASE + 0x80)

#define PWR_PUCRx(x) (PWR_BASE + 0x20 + (x) * 8)
#define PWR_PDCRx(x) (PWR_BASE + 0x24 + (x) * 8)
