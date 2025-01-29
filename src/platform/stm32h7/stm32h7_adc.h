#pragma once

#include <stdint.h>

#define ADC1_BASE      0x40022000
#define ADC2_BASE      0x40022100

#define ADC12_ISR      0x00
#define ADC12_IER      0x04
#define ADC12_CR       0x08
#define ADC12_CFGR     0x0c
#define ADC12_CFGR2    0x10
#define ADC12_SMPR1    0x14
#define ADC12_SMPR2    0x18
#define ADC12_PCSEL    0x1c
#define ADC12_LTR1     0x20
#define ADC12_HTR1     0x24

#define ADC12_SQR1     0x30
#define ADC12_SQR2     0x34
#define ADC12_SQR3     0x38
#define ADC12_SQR4     0x3c
#define ADC12_SQRx(x) (0x30 + (x) * 4)

#define ADC12_DR       0x40
#define ADC12_JSQR     0x4c
#define ADC12_OFR1     0x60
#define ADC12_OFR2     0x64
#define ADC12_OFR3     0x68
#define ADC12_OFR4     0x6c
#define ADC12_JDR1     0x80
#define ADC12_JDR2     0x84
#define ADC12_JDR3     0x88
#define ADC12_JDR4     0x8c
#define ADC12_AWD2CR   0xa0
#define ADC12_AWD3CR   0xa4
#define ADC12_LTR2     0xb0
#define ADC12_HTR2     0xb4
#define ADC12_LTR3     0xb8
#define ADC12_HTR3     0xbc
#define ADC12_DIFSEL   0xc0
#define ADC12_CALFACT  0xc4
#define ADC12_CALFACT2 0xc8

#define ADC12_CSR      0x40022300
#define ADC12_CCR      0x40022308
#define ADC12_CDR      0x4002230c


void stm32h7_adc_init(uint32_t base);

void stm32h7_adc_set_smpr(uint32_t base, uint32_t channel, uint32_t value);

void stm32h7_set_seq_channel(uint32_t base, uint32_t seq, uint32_t channel);

void stm32h7_set_seq_length(uint32_t base, uint32_t length);

uint32_t stm32h7_adc_set_channels(uint32_t base, uint32_t channels);

int stm32h7_adc_read_channel(uint32_t base, int channel);

