#pragma once

#include <stdint.h>
#include <stddef.h>
#include "stm32g0_dma.h"

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

void stm32g0_adc_multi(uint32_t channels,
                       int smpr,
                       uint16_t *output,
                       size_t num_buffers,
                       uint8_t trig,
                       int oversampling,
                       dma_cb_t *cb,
                       void *arg);

#define ADC_TRG_TIM1_TRGO2 0
#define ADC_TRG_TIM1_CC4   1
#define ADC_TRG_Res        2
#define ADC_TRG_TIM3_TRGO  3
#define ADC_TRG_TIM15_TRGO 4
#define ADC_TRG_TIM6_TRGO  5
#define ADC_TRG_TIM4_TRGO  6
#define ADC_TRG_EXTI11     7

void stm32g0_adc_multi_trig(void);
