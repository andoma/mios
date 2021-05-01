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

#define ADC_CONFIG   ((1 << 16) | (1 << 23))

static mutex_t adc_mutex = MUTEX_INITIALIZER("adc");
static uint8_t adc_initialized;


void
adc_set_smpr(uint32_t adc_base, uint32_t channel, uint32_t value)
{
  if(channel < 10) {
    reg_set_bits(adc_base + ADCx_SMPR2, channel * 3, 3, value);
  } else {
    reg_set_bits(adc_base + ADCx_SMPR1, (channel - 10) * 3, 3, value);
  }
}


static void
adc_init_one(uint32_t base, int sample_time)
{
  reg_wr(base + ADCx_SR, 0);

  reg_wr(base + ADCx_SQR1, 0);
  reg_wr(base + ADCx_SQR2, 0);
  reg_set_bit(base + ADCx_CR2, 0);

  reg_wr(base + ADCx_SMPR1, sample_time * 0b1001001001001001001001001);
  reg_wr(base + ADCx_SMPR2, sample_time * 0b1001001001001001001001001001);
}


static void
adc_init(void)
{
  clk_enable(CLK_ADC1);
  clk_enable(CLK_ADC2);
  clk_enable(CLK_ADC3);

  adc_init_one(ADC1_BASE, 7);
  adc_init_one(ADC2_BASE, 0);
  adc_init_one(ADC3_BASE, 0);

  reg_wr(ADC_CCR, ADC_CONFIG);
}


static uint16_t
adc_read_channel(int channel)
{
  mutex_lock(&adc_mutex);

  if(!adc_initialized) {
    adc_init();
    adc_initialized = 1;
  }

  reg_wr(ADC1_BASE + ADCx_SR, 0);
  reg_wr(ADC1_BASE + ADCx_SQR3, channel);

  reg_set_bit(ADC1_BASE + ADCx_CR2, 30);

  while(1) {
    if(reg_rd(ADC1_BASE + ADCx_SR) & 0x2)
      break;
  }
  int r = reg_rd(ADC1_BASE + ADCx_DR);
  mutex_unlock(&adc_mutex);
  return r;
}



int
stm32f4_adc_vref(void)
{
  return 1210 * 4096 / adc_read_channel(17);
}



static int
cmd_vref(cli_t *cli, int argc, char **argv)
{
  printf("ADC Reference voltage: %d mV\n",
         stm32f4_adc_vref());
  return 0;
}

CLI_CMD_DEF("vref", cmd_vref);
