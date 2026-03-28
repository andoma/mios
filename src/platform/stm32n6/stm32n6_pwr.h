#pragma once

#include "stm32n6_reg.h"

#define PWR_BASE 0x56024800

#define PWR_SVMCR3 (PWR_BASE + 0x3c)
#define PWR_VOSCR  (PWR_BASE + 0x20)

#define STM32N6_VDDIO_3V3 0
#define STM32N6_VDDIO_1V8 1

static inline void
stm32n6_pwr_vddio3_enable(int voltage)
{
  if(voltage == STM32N6_VDDIO_1V8)
    reg_set_bit(PWR_SVMCR3, 26); // VDDIO3VRSEL
  reg_set_bit(PWR_SVMCR3, 9);    // VDDIO3SV
  reg_set_bit(PWR_SVMCR3, 1);    // VDDIO3VMEN
}

static inline void
stm32n6_pwr_usb33_enable(void)
{
  reg_set_bit(PWR_SVMCR3, 10);   // USB33SV
  reg_set_bit(PWR_SVMCR3, 2);    // USB33VMEN
}
