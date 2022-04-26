#pragma once

#define PWR_BASE 0x58000400

#define PWR_CR1 (PWR_BASE + 0x00)
#define PWR_CR2 (PWR_BASE + 0x04)
#define PWR_CR3 (PWR_BASE + 0x08)
#define PWR_CR4 (PWR_BASE + 0x0c)
#define PWR_SR1 (PWR_BASE + 0x10)
#define PWR_SR2 (PWR_BASE + 0x14)
#define PWR_SCR (PWR_BASE + 0x18)
#define PWR_CR5 (PWR_BASE + 0x1c)

#define PWR_C2R1 (PWR_BASE + 0x80)
#define PWR_C2R3 (PWR_BASE + 0x84)
