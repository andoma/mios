#pragma once

#include <stdint.h>

#include "stm32h7_dma.h"

#define ADC1_BASE      0x40022000
#define ADC2_BASE      0x40022100
#define ADC3_BASE      0x58026000

#define ADCX_ISR      0x00
#define ADCX_IER      0x04
#define ADCX_CR       0x08
#define ADCX_CFGR     0x0c
#define ADCX_CFGR2    0x10
#define ADCX_SMPR1    0x14
#define ADCX_SMPR2    0x18
#define ADCX_PCSEL    0x1c
#define ADCX_LTR1     0x20
#define ADCX_HTR1     0x24

#define ADCX_SQR1     0x30
#define ADCX_SQR2     0x34
#define ADCX_SQR3     0x38
#define ADCX_SQR4     0x3c
#define ADCX_SQRx(x) (0x30 + (x) * 4)

#define ADCX_DR       0x40
#define ADCX_JSQR     0x4c
#define ADCX_OFR1     0x60
#define ADCX_OFR2     0x64
#define ADCX_OFR3     0x68
#define ADCX_OFR4     0x6c
#define ADCX_JDR1     0x80
#define ADCX_JDR2     0x84
#define ADCX_JDR3     0x88
#define ADCX_JDR4     0x8c
#define ADCX_AWD2CR   0xa0
#define ADCX_AWD3CR   0xa4

#define ADC12_LTR2     0xb0
#define ADC12_HTR2     0xb4
#define ADC12_LTR3     0xb8
#define ADC12_HTR3     0xbc
#define ADC12_DIFSEL   0xc0
#define ADC12_CALFACT  0xc4
#define ADC12_CALFACT2 0xc8

#define ADC3_DIFSEL   0xb0
#define ADC3_CALFACT  0xb4

#define ADCX_CSR      0x40022300
#define ADCX_CCR      0x40022308
#define ADCX_CDR      0x4002230c


void stm32h7_adc_init(uint32_t base, uint32_t pcsel, uint32_t difsel);

void stm32h7_adc_set_smpr(uint32_t base, uint32_t channel, uint32_t value);

void stm32h7_set_seq_channel(uint32_t base, uint32_t seq, uint32_t channel);

void stm32h7_set_seq_length(uint32_t base, uint32_t length);

uint32_t stm32h7_adc_set_channels(uint32_t base, uint32_t channels);

int stm32h7_adc_read_channel(uint32_t base, int channel);

stm32_dma_instance_t stm32h7_adc_init_dma(uint32_t base, uint32_t channels);
