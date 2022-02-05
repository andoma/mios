#pragma once

#include <mios/error.h>

#define ADC123_IN0   GPIO_PA(0)
#define ADC123_IN1   GPIO_PA(1)
#define ADC123_IN2   GPIO_PA(2)
#define ADC123_IN3   GPIO_PA(3)
#define ADC12_IN4    GPIO_PA(4)
#define ADC12_IN5    GPIO_PA(5)
#define ADC12_IN6    GPIO_PA(6)
#define ADC12_IN7    GPIO_PA(7)
#define ADC12_IN8    GPIO_PB(0)
#define ADC12_IN9    GPIO_PB(1)

#define ADC3_IN4     GPIO_PF(6)
#define ADC3_IN5     GPIO_PF(7)
#define ADC3_IN6     GPIO_PF(8)
#define ADC3_IN7     GPIO_PF(9)
#define ADC3_IN8     GPIO_PF(10)
#define ADC3_IN9     GPIO_PF(3)

#define ADC123_IN10  GPIO_PC(0)
#define ADC123_IN11  GPIO_PC(1)
#define ADC123_IN12  GPIO_PC(2)
#define ADC123_IN13  GPIO_PC(3)

#define ADC12_IN14   GPIO_PC(4)
#define ADC12_IN15   GPIO_PC(5)

#define ADC3_IN14    GPIO_PF(4)
#define ADC3_IN15    GPIO_PF(5)





#define ADCx_BASE(x) (0x40012000 + (x) * 0x100)

#define ADC1_BASE 0x40012000
#define ADC2_BASE 0x40012100
#define ADC3_BASE 0x40012200


#define ADCx_SR     0x00
#define ADCx_CR1    0x04
#define ADCx_CR2    0x08
#define ADCx_SMPR1  0x0c
#define ADCx_SMPR2  0x10
#define ADCx_JOFR1  0x14
#define ADCx_JOFR2  0x18
#define ADCx_JOFR3  0x1c
#define ADCx_JOFR4  0x20
#define ADCx_SQR1   0x2c
#define ADCx_SQR2   0x30
#define ADCx_SQR3   0x34
#define ADCx_JSQR   0x38
#define ADCx_JDR1   0x3c
#define ADCx_JDR2   0x40
#define ADCx_JDR3   0x44
#define ADCx_JDR4   0x48
#define ADCx_DR     0x4c


#define ADCC_BASE 0x40012300
#define ADC_CCR (ADCC_BASE + 0x04)

int stm32f4_adc_vref(void); // Return vref in mV

void stm32f4_adc_set_smpr(uint32_t adc_base, uint32_t channel, uint32_t value);

uint32_t stm32f4_adc_init(int unit, int mode);

uint16_t adc_read_channel(int channel);

uint32_t stm32f4_adc_init_dma(int unit, uint32_t mask);

error_t stm32f4_adc_dma_transfer(int unit, uint32_t dmainst, uint16_t *output);
