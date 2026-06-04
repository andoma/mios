#pragma once

#include <stdint.h>

// STM32N6 has two 12-bit ADCs (ADC1 = master, ADC2 = slave) sharing a
// common register block.  The SAR IP is the same family as the STM32H7,
// so most per-ADC register offsets match.  Notable differences:
//   - 12-bit only, ADC1/ADC2 only (no ADC3)
//   - No analog voltage regulator enable (no ADVREGEN / LDORDY)
//   - Kernel clock prescaler lives in RCC (RCC_CCIPR1), not in the CCR
//   - Offset calibration is a software-driven procedure (RM 32.4.8),
//     not implemented here yet (conversions work without it, with a
//     small offset error).

#define ADC1_BASE      0x40022000
#define ADC2_BASE      0x40022100

// Per-ADC registers (offset from ADCx_BASE)
#define ADCX_ISR       0x00
#define ADCX_IER       0x04
#define ADCX_CR        0x08
#define ADCX_CFGR1     0x0c
#define ADCX_CFGR2     0x10
#define ADCX_SMPR1     0x14
#define ADCX_SMPR2     0x18
#define ADCX_PCSEL     0x1c
#define ADCX_SQR1      0x30
#define ADCX_SQR2      0x34
#define ADCX_SQR3      0x38
#define ADCX_SQR4      0x3c
#define ADCX_SQRx(x)   (0x30 + (x) * 4)
#define ADCX_DR        0x40
#define ADCX_JSQR      0x4c
#define ADCX_DIFSEL    0xc0
#define ADCX_CALFACT   0xc4
#define ADCX_OR        0xd0

// ADCX_ISR / ADCX_IER bits
#define ADCX_ISR_ADRDY  0
#define ADCX_ISR_EOSMP  1
#define ADCX_ISR_EOC    2
#define ADCX_ISR_EOS    3
#define ADCX_ISR_OVR    4

// ADCX_CR bits
#define ADCX_CR_ADEN     0
#define ADCX_CR_ADDIS    1
#define ADCX_CR_ADSTART  2
#define ADCX_CR_ADSTP    4
#define ADCX_CR_DEEPPWD  29
#define ADCX_CR_ADCALDIF 30
#define ADCX_CR_ADCAL    31

// ADCX_OR bits
#define ADCX_OR_OP0      0  // Internal reference buffer enable
#define ADCX_OR_OP1      1  // ADC internal bandgap enable
#define ADCX_OR_OP2      2  // VDDCORE channel enable

// Common register block
#define ADCC_BASE      0x40022300
#define ADCC_CSR       (ADCC_BASE + 0x000)
#define ADCC_CCR       (ADCC_BASE + 0x008)
#define ADCC_CDR       (ADCC_BASE + 0x00c)

// ADCC_CCR bits
#define ADCC_CCR_VREFEN  22  // Enable VREFINT channel  (ADC1 ch17)
#define ADCC_CCR_VBATEN  24  // Enable VBAT/4 channel   (ADC2 ch16)

// Internal channel numbers (see RM Table 242 / ADC connectivity)
#define ADC1_CH_VREFINT  17
#define ADC2_CH_VBAT     16  // VBAT / 4
#define ADC2_CH_VDDCORE  17
#define ADCX_CH_VREF     19  // VREF+ (both ADCs)

// VREF+ supplied externally on these boards.
#define ADC_VREF_MV 1800

// Internal voltage reference buffer (VREFBUF). On the N6 the ADC reference
// VREF+ must be sourced externally, or driven internally by the VREFBUF.
#define VREFBUF_BASE   0x46003c00
#define VREFBUF_CSR    (VREFBUF_BASE + 0x00)
#define VREFBUF_CSR_ENVR 0  // Buffer mode enable
#define VREFBUF_CSR_HIZ  1  // High-impedance mode
#define VREFBUF_CSR_VRR  3  // Buffer ready (read-only)
// VRS[6:4]: 0 => VREFBUF0 (~1.21 V), 1 => VREFBUF1 (~1.5 V)
#define VREFBUF_VRS    1
#define VREFBUF_VREF_MV 1500

// Drive VREF+ from the internal VREFBUF (VREFBUF1, ~1.5 V) and wait until
// ready. Only use this when VREF+ is NOT supplied externally on the board.
void stm32n6_vrefbuf_enable(void);

// Bring ADC12 out of deep-power-down and enable it. pcsel is the channel
// preselect mask (bit N = channel N's I/O analog switch); difsel selects
// differential channels (0 = all single-ended). Both must be programmed
// before ADEN is set, so they are passed in here. Idempotent.
void stm32n6_adc_init(uint32_t base, uint32_t pcsel, uint32_t difsel);

// Set sampling time (SMP[2:0], 0..7) for a channel.
void stm32n6_adc_set_smpr(uint32_t base, uint32_t channel, uint32_t value);

// Single blocking conversion of one channel. Returns the raw 12-bit code.
int stm32n6_adc_read_channel(uint32_t base, int channel);

// Configure an external (GPIO) input channel: enable its I/O analog switch
// (PCSEL) and set a long sampling time. The pin must separately be put in
// analog mode with gpio_conf_analog().
void stm32n6_adc_config_input(uint32_t base, int channel);

// Enable internal analog sources.
void stm32n6_adc_enable_vrefint(void);   // ADC1 ch17
void stm32n6_adc_enable_vbat(void);      // ADC2 ch16
void stm32n6_adc_enable_vddcore(void);   // ADC2 ch17
