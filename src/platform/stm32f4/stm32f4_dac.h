#pragma once

#define DAC_BASE 0x40007400

#define DAC_CR            (DAC_BASE + 0x00)

#define DAC_DHR12R1      (DAC_BASE + 0x08)
#define DAC_DHR12L1      (DAC_BASE + 0x0c)
#define DAC_DHR8R1       (DAC_BASE + 0x10)
#define DAC_DHR12R2      (DAC_BASE + 0x14)
#define DAC_DHR12L2      (DAC_BASE + 0x18)
#define DAC_DHR8R2       (DAC_BASE + 0x1c)
