#include "stm32h7_adc.h"
#include "stm32h7_clk.h"

#include <mios/io.h>
#include <mios/mios.h>
#include <mios/task.h>

#include <assert.h>
#include <unistd.h>
#include <stdio.h>

#include "irq.h"

static void
stm32h7_adc_init_clk(uint16_t clk)
{
  if(clk_is_enabled(clk))
    return;

  int f = clk_get_freq(clk);
  int maxf = 50000000; // Depends on voltage scaling (assumes VOS=3)

  int presc = 0;
  while(1) {
    if((f >> presc) <= maxf || presc == 11)
      break;
    presc++;
  }

  reg_set_bits(ADCX_CCR, 18, 4, presc);
  reg_set_bits(ADCX_CCR, 16, 2, 0);
  printf("adc1/2: Input clock: %d / %d (presc=%d) => %d Hz\n",
	 f, 1 << presc, presc, f >> presc);

  clk_enable(clk);
  udelay(1);
}


void
stm32h7_adc_init(uint32_t base, uint32_t pcsel, uint32_t difsel)
{
  const char *name = NULL;
  switch(base) {
  case ADC1_BASE:
    name = "adc1";
    stm32h7_adc_init_clk(CLK_ADC12);
    break;
  case ADC2_BASE:
    name = "adc2";
    stm32h7_adc_init_clk(CLK_ADC12);
    break;
  case ADC3_BASE:
    name = "adc3";
    stm32h7_adc_init_clk(CLK_ADC3);
    break;
  default:
    panic("Unsupported ADC");
  }

  if(reg_get_bit(base + ADCX_CR, 29) == 0)
    return; // Already initialized

  reg_clr_bit(base + ADCX_CR, 29);  // Turn off DEEPPWD
  reg_set_bit(base + ADCX_CR, 28);  // Turn on voltage regulator

  // Wait for LDO to become ready
  if (base != ADC3_BASE) {
    while(!reg_get_bit(base + ADCX_ISR, 12)) {};
  } else {
    udelay(10); // bit unavailable in ADC 3, max setup time is 10 us.
  }
  reg_set_bits(base + ADCX_CR, 8, 2, 0b11);

  // Single-ended and Linearity calibration, linearity not available on ADC3
  if (base != ADC3_BASE)
    reg_set_bit(base + ADCX_CR, 16);

  reg_set_bit(base + ADCX_CR, 31);
  while(reg_get_bit(base + ADCX_CR, 31)) {}
  reg_clr_bit(base + ADCX_CR, 16);

  if(difsel) {
    // Differential calibration
    reg_set_bit(base + ADCX_CR, 30);
    reg_set_bit(base + ADCX_CR, 31);
    while(reg_get_bit(base + ADCX_CR, 31)) {}
    reg_clr_bit(base + ADCX_CR, 30);
  }
  if (base != ADC3_BASE)
    reg_wr(base + ADC12_DIFSEL, difsel);
  else
    reg_wr(base + ADC3_DIFSEL, difsel);

  if (base != ADC3_BASE)
    reg_wr(base + ADCX_PCSEL, pcsel);
  else
    reg_set_bits(base + ADCX_CFGR, 15, 1, 1); // left align

  reg_set_bit(base + ADCX_CR, 0); // Turn on ADEN
  while(reg_get_bit(base + ADCX_ISR, 0) == 0) { }

  printf("%s: Calibration: 0x%08x\n", name, base != ADC3_BASE ?
	 reg_rd(base + ADC12_CALFACT) :
	 reg_rd(base + ADC3_CALFACT));
}


void
stm32h7_adc_set_smpr(uint32_t base, uint32_t channel, uint32_t value)
{
  if(channel < 10) {
    reg_set_bits(base + ADCX_SMPR1, channel * 3, 3, value);
  } else {
    reg_set_bits(base + ADCX_SMPR2, (channel - 10) * 3, 3, value);
  }
}


void
stm32h7_set_seq_channel(uint32_t base, uint32_t seq, uint32_t channel)
{
  const uint32_t reg = seq / 5;
  const uint32_t bo = seq % 5;
  reg_set_bits(base + ADCX_SQRx(reg), bo * 6, 6, channel);
}


void
stm32h7_set_seq_length(uint32_t base, uint32_t length)
{
  reg_set_bits(base + ADCX_SQRx(0), 0, 4, length - 1);
}


uint32_t
stm32h7_adc_set_channels(uint32_t base, uint32_t channels)
{
  int seq = 0;
  for(int i = 0; i < 32; i++) {
    if((1 << i) & channels) {
      stm32h7_set_seq_channel(base, seq + 1, i);
      seq++;
    }
  }
  assert(seq > 0);
  stm32h7_set_seq_length(base, seq);
  return seq;
}


static mutex_t adc_mtx = MUTEX_INITIALIZER("adc");

int
stm32h7_adc_read_channel(uint32_t base, int channel)
{
  mutex_lock(&adc_mtx);
  reg_wr(base + ADCX_SQR1, channel << 6);
  reg_set_bit(base + ADCX_CR, 2);

  while(1) {
    if(reg_get_bit(base + ADCX_ISR, 2))
      break;
  }
  int ret = reg_rd(base + ADCX_DR);
  mutex_unlock(&adc_mtx);
  return ret;
}


stm32_dma_instance_t
stm32h7_adc_init_dma(uint32_t base, uint32_t channels)
{
  uint32_t seqlen = stm32h7_adc_set_channels(base, channels);

  stm32_dma_instance_t dmainst;
  switch(base) {
  case ADC1_BASE:
    dmainst = stm32_dma_alloc(9, "adc1");
    break;
  case ADC2_BASE:
    dmainst = stm32_dma_alloc(10, "adc2");
    break;
  default:
    panic("Unsupported ADC");
  }

  stm32_dma_config(dmainst,
                   STM32_DMA_BURST_NONE,
                   STM32_DMA_BURST_NONE,
                   STM32_DMA_PRIO_VERY_HIGH,
                   STM32_DMA_16BIT,
                   STM32_DMA_16BIT,
                   STM32_DMA_INCREMENT,
                   STM32_DMA_FIXED,
                   STM32_DMA_CIRCULAR,
                   STM32_DMA_P_TO_M);

  stm32_dma_set_nitems(dmainst, seqlen);

  stm32_dma_set_paddr(dmainst, base + ADCX_DR);

  reg_set_bits(base + ADCX_CFGR, 0, 2, 0b11); // DMA mode
  return dmainst;
}
