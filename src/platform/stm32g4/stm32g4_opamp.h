#pragma once

#define OPAMP_BASE 0x40010300

#define OPAMP_CSR(x) (OPAMP_BASE + ((x) - 1) * 4)
