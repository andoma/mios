#include "stm32n6_adc.h"
#include "stm32n6_clk.h"
#include "stm32n6_pwr.h"

#include <mios/cli.h>
#include <mios/io.h>
#include <mios/mios.h>
#include <mios/task.h>

#include <stdio.h>
#include <unistd.h>

// Maximum ADC kernel clock (adc_ker_ck), see datasheet (fADC = 70 MHz).
#define ADC_KER_CK_MAX 70000000

static void
stm32n6_adc_init_clk(void)
{
  if(clk_is_enabled(CLK_ADC12))
    return;

  // The ADC analog supply (VDDA18ADC) is isolated at reset; power it.
  stm32n6_pwr_vdda_enable();

  // ADC12SEL defaults to hclk1; clk_get_freq() returns that bus frequency.
  // Program ADCPRE[7:0] (linear divider, ck_ker_adc12 / (v + 1)) so the
  // kernel clock stays within fADC.
  unsigned int f = clk_get_freq(CLK_ADC12);
  unsigned int presc = 0;
  while(f / (presc + 1) > ADC_KER_CK_MAX && presc < 255)
    presc++;

  reg_set_bits(RCC_CCIPR1, 8, 8, presc); // ADCPRE
  printf("adc12: kernel clock %u / %u => %u Hz\n",
         f, presc + 1, f / (presc + 1));

  clk_enable(CLK_ADC12);
  udelay(1);
}

void
stm32n6_adc_init(uint32_t base, uint32_t pcsel, uint32_t difsel)
{
  stm32n6_adc_init_clk();

  if(reg_get_bit(base + ADCX_CR, ADCX_CR_ADEN))
    return; // Already enabled

  // Exit deep-power-down. The N6 ADC has no software voltage regulator
  // (no ADVREGEN / LDORDY); a short stabilization delay is enough.
  reg_clr_bit(base + ADCX_CR, ADCX_CR_DEEPPWD);
  udelay(10);

  // NOTE: offset calibration (RM 32.4.8) is not run here yet. Conversions
  // work without it, with a small (few LSB) offset error.

  // Channel preselect and differential selection must be programmed before
  // the ADC is enabled (matches the STM32H7 sequence; the I/O analog switch
  // latches its selection at enable time).
  reg_wr(base + ADCX_DIFSEL, difsel);
  reg_wr(base + ADCX_PCSEL, pcsel);

  // Enable the ADC and wait until it is ready.
  reg_set_bit(base + ADCX_ISR, ADCX_ISR_ADRDY); // Clear ADRDY (write 1)
  reg_set_bit(base + ADCX_CR, ADCX_CR_ADEN);
  while(!reg_get_bit(base + ADCX_ISR, ADCX_ISR_ADRDY)) {}
}

void
stm32n6_adc_set_smpr(uint32_t base, uint32_t channel, uint32_t value)
{
  if(channel < 10) {
    reg_set_bits(base + ADCX_SMPR1, channel * 3, 3, value);
  } else {
    reg_set_bits(base + ADCX_SMPR2, (channel - 10) * 3, 3, value);
  }
}

void
stm32n6_adc_config_input(uint32_t base, int channel)
{
  // PCSEL is programmed in stm32n6_adc_init() before enable; here we only set
  // the per-channel sampling time.
  stm32n6_adc_set_smpr(base, channel, 6); // 246.5 cycles (SMP=7 misbehaves)
}

static mutex_t adc_mtx = MUTEX_INITIALIZER("adc");

int
stm32n6_adc_read_channel(uint32_t base, int channel)
{
  mutex_lock(&adc_mtx);

  // Single conversion: sequence length 1 (L=0), SQ1 = channel.
  reg_wr(base + ADCX_SQR1, channel << 6);
  reg_set_bit(base + ADCX_CR, ADCX_CR_ADSTART);

  while(!reg_get_bit(base + ADCX_ISR, ADCX_ISR_EOC)) {}

  int ret = reg_rd(base + ADCX_DR);
  mutex_unlock(&adc_mtx);
  return ret;
}

void
stm32n6_adc_enable_vrefint(void)
{
  stm32n6_adc_init(ADC1_BASE, 0, 0);
  reg_set_bit(ADCC_CCR, ADCC_CCR_VREFEN);
  stm32n6_adc_set_smpr(ADC1_BASE, ADC1_CH_VREFINT, 6);
  udelay(20); // VREFINT startup
}

void
stm32n6_adc_enable_vbat(void)
{
  stm32n6_adc_init(ADC2_BASE, 0, 0);
  reg_set_bit(ADCC_CCR, ADCC_CCR_VBATEN);
  stm32n6_adc_set_smpr(ADC2_BASE, ADC2_CH_VBAT, 6);
}

void
stm32n6_adc_enable_vddcore(void)
{
  stm32n6_adc_init(ADC2_BASE, 0, 0);
  reg_set_bit(ADC2_BASE + ADCX_OR, ADCX_OR_OP2);
  stm32n6_adc_set_smpr(ADC2_BASE, ADC2_CH_VDDCORE, 6);
}

void
stm32n6_vrefbuf_enable(void)
{
  clk_enable(CLK_VREFBUF);

  if(reg_get_bit(VREFBUF_CSR, VREFBUF_CSR_ENVR) &&
     reg_get_bit(VREFBUF_CSR, VREFBUF_CSR_VRR))
    return; // Already enabled and ready

  // VRS can only be programmed while the buffer is disabled.
  reg_clr_bit(VREFBUF_CSR, VREFBUF_CSR_ENVR);
  reg_set_bits(VREFBUF_CSR, 4, 3, VREFBUF_VRS);

  // Internal reference mode: buffer on, VREF+ driven from VREFINT.
  reg_clr_bit(VREFBUF_CSR, VREFBUF_CSR_HIZ);
  reg_set_bit(VREFBUF_CSR, VREFBUF_CSR_ENVR);

  while(!reg_get_bit(VREFBUF_CSR, VREFBUF_CSR_VRR)) {}
}

static error_t
cmd_adc(cli_t *cli, int argc, char **argv)
{
  // VREF+ is supplied externally (1.8 V) on these boards: do NOT drive the
  // internal VREFBUF.
  stm32n6_adc_enable_vrefint();
  stm32n6_adc_enable_vbat();
  stm32n6_adc_enable_vddcore();

  const int vref_mv = ADC_VREF_MV;

  int vrefint = stm32n6_adc_read_channel(ADC1_BASE, ADC1_CH_VREFINT);
  int vbat_raw = stm32n6_adc_read_channel(ADC2_BASE, ADC2_CH_VBAT);
  int vddcore_raw = stm32n6_adc_read_channel(ADC2_BASE, ADC2_CH_VDDCORE);

  cli_printf(cli, "VREF+   : %d mV (external)\n", vref_mv);
  cli_printf(cli, "VREFINT : raw=%-5d => %d mV\n",
             vrefint, vrefint * vref_mv / 4095);
  cli_printf(cli, "VBAT    : raw=%-5d => %d mV\n",
             vbat_raw, 4 * vbat_raw * vref_mv / 4095);
  cli_printf(cli, "VDDCORE : raw=%-5d => %d mV\n",
             vddcore_raw, vddcore_raw * vref_mv / 4095);
  return 0;
}

CLI_CMD_DEF_EXT("adc", cmd_adc, NULL, "Read internal ADC channels");
