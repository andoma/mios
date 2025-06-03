#pragma once

#define OPAMP_BASE 0x40009000

#define OPAMP1_CSR(x) (OPAMP_BASE + ((x) - 1) * 0x10)
