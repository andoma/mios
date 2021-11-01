#pragma once

#define ADC_BASE  0x40012400

#define ADC_ISR     (ADC_BASE + 0x00)
#define ADC_IER     (ADC_BASE + 0x04)
#define ADC_CR      (ADC_BASE + 0x08)
#define ADC_CFGR1   (ADC_BASE + 0x0c)
#define ADC_CFGR2   (ADC_BASE + 0x10)
#define ADC_SMPR    (ADC_BASE + 0x14)
#define ADC_AWD1TR  (ADC_BASE + 0x20)
#define ADC_AWD2TR  (ADC_BASE + 0x24)
#define ADC_CHSELR  (ADC_BASE + 0x28)
#define ADC_DR      (ADC_BASE + 0x40)
#define ADC_CALFACT (ADC_BASE + 0xb4)
#define ADC_CCR     (ADC_BASE + 0x308)

int stm32g0_adc_vref(void); // Return vref in mV

int adc_read_channel(int channel);
