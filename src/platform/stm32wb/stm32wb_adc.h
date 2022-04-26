#pragma once

#define ADC_BASE 0x50040000

#define ADC_ISR     (ADC_BASE + 0x00)
#define ADC_IER     (ADC_BASE + 0x04)
#define ADC_CR      (ADC_BASE + 0x08)
#define ADC_CFGR1   (ADC_BASE + 0x0c)
#define ADC_CFGR2   (ADC_BASE + 0x10)
#define ADC_SMPR1   (ADC_BASE + 0x14)
#define ADC_SMPR2   (ADC_BASE + 0x18)
#define ADC_TR1     (ADC_BASE + 0x20)
#define ADC_TR2     (ADC_BASE + 0x24)
#define ADC_TR3     (ADC_BASE + 0x28)
#define ADC_SQR1    (ADC_BASE + 0x30)
#define ADC_SQR2    (ADC_BASE + 0x34)
#define ADC_SQR3    (ADC_BASE + 0x38)
#define ADC_SQR4    (ADC_BASE + 0x3c)
#define ADC_DR      (ADC_BASE + 0x40)
#define ADC_CALFACT (ADC_BASE + 0xb4)
#define ADC_CCR     (ADC_BASE + 0x308)
