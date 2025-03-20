#include <stdint.h>
#include <mios/error.h>
#include <mios/cli.h>
#include <mios/io.h>
#include <mios/task.h>
#include <stdio.h>
#include <unistd.h>

#include "irq.h"
#include "stm32f4_reg.h"
#include "stm32f4_clk.h"
#include "stm32f4_adc.h"
#include "stm32f4_dma.h"

#define ADC_CONFIG   ((1 << 16) | (1 << 23))

static mutex_t adc_mutex = MUTEX_INITIALIZER("adc");
static uint8_t adc_initialized[3];

void
stm32f4_adc_set_smpr(uint32_t adc_base, uint32_t channel, uint32_t value)
{
  if(channel < 10) {
    reg_set_bits(adc_base + ADCx_SMPR2, channel * 3, 3, value);
  } else {
    reg_set_bits(adc_base + ADCx_SMPR1, (channel - 10) * 3, 3, value);
  }
}

static void
stm32f4_adc_set_sqr(uint32_t adc_base, uint32_t index, uint32_t channel)
{
  if(index < 6)
    reg_set_bits(adc_base + ADCx_SQR3, index * 5, 5, channel);
  else if(index < 12)
    reg_set_bits(adc_base + ADCx_SQR2, (index - 6) * 5, 5, channel);
  else if(index < 16)
    reg_set_bits(adc_base + ADCx_SQR1, (index - 12) * 5, 5, channel);
  else
    panic("ADC: Invalid index %d", index);
}

static void
stm32f4_adc_set_seq_len(uint32_t adc_base, uint32_t len)
{
  reg_set_bits(adc_base + ADCx_SQR1, 20, 4, len - 1);
}


static void
adc_init_one(uint32_t base, int sample_time)
{
  reg_wr(base + ADCx_SR, 0);

  reg_wr(base + ADCx_SQR1, 0);
  reg_wr(base + ADCx_SQR2, 0);
  reg_wr(base + ADCx_SQR3, 0);
  reg_set_bit(base + ADCx_CR2, 0);

  reg_wr(base + ADCx_SMPR1, sample_time * 0b1001001001001001001001001);
  reg_wr(base + ADCx_SMPR2, sample_time * 0b1001001001001001001001001001);
}


uint32_t
stm32f4_adc_init(int unit, int mode)
{
  if(unit < 1 || unit > 3)
    panic("Invalid ADC unit %d", unit);

  unit--;

  if(adc_initialized[unit] != mode) {

    if(adc_initialized[unit] == 0) {
      adc_initialized[unit] = mode;
      clk_enable(CLK_ADCx(unit));
      adc_init_one(ADCx_BASE(unit), 7);
      reg_wr(ADC_CCR, ADC_CONFIG);
    } else {
      return 0;
    }
  }
  return ADCx_BASE(unit);
}


uint16_t
adc_read_channel(int unit, int channel)
{
  mutex_lock(&adc_mutex);

  int r = 0;
  const uint32_t base = stm32f4_adc_init(unit, 1);
  if(base) {
    reg_wr(base + ADCx_SR, 0);
    reg_wr(base + ADCx_SQR3, channel);

    reg_set_bit(base + ADCx_CR2, 30);

    while(1) {
      if(reg_rd(base + ADCx_SR) & 0x2)
        break;
    }
    r = reg_rd(base + ADCx_DR);
  }
  mutex_unlock(&adc_mutex);
  return r;
}


int
stm32f4_adc_vref(void)
{
  return 1210 * 4096 / adc_read_channel(1, 17);
}



static error_t
cmd_vref(cli_t *cli, int argc, char **argv)
{
  cli_printf(cli, "ADC Reference voltage: %d mV\n",
             stm32f4_adc_vref());
  return 0;
}

CLI_CMD_DEF("vref", cmd_vref);


static const uint32_t adc_dma_resources[3] = {
  STM32F4_DMA_ADC1, STM32F4_DMA_ADC2, STM32F4_DMA_ADC3
};




uint32_t
stm32f4_adc_init_dma(int unit, uint32_t mask)
{
  mutex_lock(&adc_mutex);

  const uint32_t adc_base = stm32f4_adc_init(unit, 2);

  if(!adc_base)
    panic("Conflicting ADC modes");

  uint32_t seqlen = 0;
  for(int i = 0; i < 18; i++) {
    if((1 << i) & mask) {
      stm32f4_adc_set_sqr(adc_base, seqlen, i);
      seqlen++;
    }
  }
  unit--;

  stm32f4_adc_set_seq_len(adc_base, seqlen);

  stm32_dma_instance_t dmainst = stm32_dma_alloc(adc_dma_resources[unit], "adc");

  stm32_dma_make_waitable(dmainst, "adc");

  stm32_dma_config(dmainst,
                   STM32_DMA_BURST_NONE,
                   STM32_DMA_BURST_NONE,
                   STM32_DMA_PRIO_VERY_HIGH,
                   STM32_DMA_16BIT,
                   STM32_DMA_16BIT,
                   STM32_DMA_INCREMENT,
                   STM32_DMA_FIXED,
                   STM32_DMA_SINGLE,
                   STM32_DMA_P_TO_M);

  stm32_dma_set_paddr(dmainst, adc_base + ADCx_DR);
  stm32_dma_set_nitems(dmainst, seqlen);

  reg_set_bit(adc_base + ADCx_CR1, 8); // Scan mode
  reg_set_bit(adc_base + ADCx_CR2, 8); // DMA mode
  reg_set_bit(adc_base + ADCx_CR2, 9);

  mutex_unlock(&adc_mutex);

  return dmainst;
}


error_t
stm32f4_adc_dma_transfer(int unit, uint32_t dmainst, uint16_t *output)
{
  const uint32_t adc_base = ADCx_BASE(unit - 1);
  stm32_dma_set_mem0(dmainst, output);

  int q = irq_forbid(IRQ_LEVEL_SCHED);
  stm32_dma_start(dmainst);
  reg_set_bit(adc_base + ADCx_CR2, 30);
  error_t error = stm32_dma_wait(dmainst);
  irq_permit(q);
  return error;
}
