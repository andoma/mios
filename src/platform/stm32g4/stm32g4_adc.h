#pragma once

#include <stdint.h>

#include "stm32g4_dma.h"

#define ADC1_BASE    0x50000000
#define ADC2_BASE    0x50000100
#define ADC3_BASE    0x50000400
#define ADC4_BASE    0x50000500
#define ADC5_BASE    0x50000600

#define ADCx_ISR     0x00
#define ADCx_IER     0x04
#define ADCx_CR      0x08
#define ADCx_CFGR    0x0c
#define ADCx_CFGR2   0x10
#define ADCx_SMPR1   0x14
#define ADCx_SMPR2   0x18
#define ADCx_TR1     0x20
#define ADCx_TR2     0x24
#define ADCx_TR3     0x28
#define ADCx_SQR1    0x30
#define ADCx_SQR2    0x34
#define ADCx_SQR3    0x38
#define ADCx_SQR4    0x3c
#define ADCx_SQRx(x)(0x30 + (x) * 4)

#define ADCx_DR      0x40
#define ADCx_JSQR    0x4c
#define ADCx_OFR1    0x60
#define ADCx_OFR2    0x64
#define ADCx_OFR3    0x68
#define ADCx_OFR4    0x6c
#define ADCx_JDR1    0x80
#define ADCx_JDR2    0x84
#define ADCx_JDR3    0x88
#define ADCx_JDR4    0x8c
#define ADCx_AWD2CR  0xa0
#define ADCx_AWD3CR  0xa4
#define ADCx_DIFSEL  0xb0
#define ADCx_CALFACT 0xb4
#define ADCx_GIMP    0xc0

#define ADC12_CSR     0x50000300
#define ADC12_CCR     0x50000308
#define ADC12_CDR     0x5000030c


void stm32g4_adc_init(uint32_t base);

void stm32g4_adc_set_smpr(uint32_t base, uint32_t channel, uint32_t value);

void stm32g4_set_seq_channel(uint32_t base, uint32_t seq, uint32_t channel);

void stm32g4_set_seq_length(uint32_t base, uint32_t length);

uint32_t stm32g4_adc_set_channels(uint32_t base, uint32_t channels);

stm32_dma_instance_t stm32g4_adc_init_dma(uint32_t base, uint32_t channels);

error_t stm32g4_adc_dma_transfer(uint32_t base, uint32_t dmainst, uint16_t *output);

int stm32g4_adc_read_channel(uint32_t base, int channel);


#define ADC12_EXT_TIM1_TRGO  9
#define ADC12_EXT_TIM1_TRGO2 10

#define ADC12_JEXT_TIM1_TRGO  0
#define ADC12_JEXT_TIM1_TRGO2 8

