#pragma once

#define DAC_BASE(x) (0x40007400 + (x) * 0x400)

#define DAC_CR        0x00
#define DAC_DHR12R1   0x08
#define DAC_DHR12R2   0x14
#define DAC_MCR       0x3c
