#pragma once

#include <stdint.h>
#include <mios/error.h>

#include "stm32n6_dma.h"
#include "stm32n6_reg.h"

#define ADC1_BASE 0x40022000
#define ADC2_BASE 0x40022100

#define ADC12_COMMON_BASE 0x40022300

#define ADCx_ISR     0x00
#define ADCx_IER     0x04
#define ADCx_CR      0x08
#define ADCx_CFGR1   0x0c
#define ADCx_CFGR2   0x10
#define ADCx_SMPR1   0x14
#define ADCx_SMPR2   0x18
#define ADCx_PCSEL   0x1c
#define ADCx_SQRx(n) (0x30 + (n) * 4)
#define ADCx_DR      0x40
#define ADCx_JSQR    0x4c
#define ADCx_JDR1    0x80
#define ADCx_DIFSEL  0xc0
#define ADCx_CALFACT 0xc4

#define ADC12_CSR    (ADC12_COMMON_BASE + 0x00)
#define ADC12_CCR    (ADC12_COMMON_BASE + 0x08)

#define ADCx_CR_ADSTART_Pos  2
#define ADCx_CR_JADSTART_Pos 3
#define ADCx_ISR_EOC_Pos     2
#define ADCx_ISR_JEOC_Pos    5

// One-time per-instance bring-up. Idempotent.
//
// `pcsel` — bitmap of channels (bit N = ch N) to mark in the
// preselection register. RM0486 §32.4.12: every channel used in
// the regular sequence MUST have its PCSEL bit set, otherwise the
// I/O analog switch stays open and the conversion result is wrong.
// For differential channel i, set both bit i and bit i-1.
//
// `difsel` — bitmap of channels in differential mode. 0 = all
// single-ended.
void stm32n6_adc_init(uint32_t base, uint32_t pcsel, uint32_t difsel);

void stm32n6_adc_set_smpr(uint32_t base, uint32_t channel, uint32_t value);

void stm32n6_adc_set_seq_channel(uint32_t base, uint32_t seq, uint32_t channel);
void stm32n6_adc_set_seq_length(uint32_t base, uint32_t length);
uint32_t stm32n6_adc_set_channels(uint32_t base, uint32_t channels);

int stm32n6_adc_read_channel(uint32_t base, int channel);

__attribute__((always_inline))
static inline void
stm32n6_adc_start(uint32_t base)
{
  reg_set_bit(base + ADCx_CR, ADCx_CR_ADSTART_Pos);
}

__attribute__((always_inline))
static inline uint16_t
stm32n6_adc_read(uint32_t base)
{
  return reg_rd(base + ADCx_DR);
}

// Injected-sequence path. Lets the regular sequence (started from
// thread context, possibly via DMA) coexist with an IRQ-triggered
// single-channel sample. Configure once with set_injected_channel,
// then trigger with inj_start() and read with inj_read() — both
// safe to call from IRQ context.
//
// Channel must have its PCSEL bit set (typically as part of
// stm32n6_adc_init's pcsel parameter).
void stm32n6_adc_set_injected_channel(uint32_t base, int channel);

__attribute__((always_inline))
static inline void
stm32n6_adc_inj_start(uint32_t base)
{
  reg_set_bit(base + ADCx_CR, ADCx_CR_JADSTART_Pos);
}

__attribute__((always_inline))
static inline uint16_t
stm32n6_adc_inj_read(uint32_t base)
{
  return reg_rd(base + ADCx_JDR1);
}

stm32_dma_instance_t stm32n6_adc_init_dma(uint32_t base, uint32_t channels);

error_t stm32n6_adc_dma_transfer(uint32_t base,
                                 stm32_dma_instance_t dmainst,
                                 uint16_t *output);
