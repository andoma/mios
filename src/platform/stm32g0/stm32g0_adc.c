#include "stm32g0_adc.h"

#include <stdint.h>
#include <mios/error.h>
#include <mios/cli.h>
#include <mios/io.h>
#include <mios/task.h>
#include <stdio.h>
#include <unistd.h>

#include "irq.h"
#include "stm32g0_reg.h"
#include "stm32g0_clk.h"
#include "stm32g0_dma.h"

static uint8_t adc_initialized;

static void
adc_init(int how)
{
  if(adc_initialized)
    return;
  adc_initialized = how;

  clk_enable(CLK_ADC);

  // Enable Voltage Regulator
  reg_wr(ADC_CR, (1 << 28));
  udelay(20);

  // Enable VREFEN
  reg_set_bit(ADC_CCR, 22);
  udelay(20);

  // Calibrate ADC
  reg_wr(ADC_CR, (1 << 31) | (1 << 28));

  while(1) {
    uint32_t r = reg_rd(ADC_CR);
    if(!(r & (1 << 31)))
      break;
  }

  // Enable ADC
  reg_wr(ADC_ISR, 0xffffffff);
  reg_wr(ADC_CR, (1 << 28) | (1 << 0));
  while(!(reg_rd(ADC_ISR) & 0x1)) {}
}


static void
set_channel_mask(uint32_t channels)
{
  reg_wr(ADC_CHSELR, channels);
  // Wait for Channel Configuration Ready flag
  while(!(reg_rd(ADC_ISR) & (1 << 13))) {}
  reg_wr(ADC_ISR, (1 << 13));
}


int
adc_read_channel(int channel)
{
  int q = irq_forbid(IRQ_LEVEL_SWITCH);
  adc_init(1);

  int r;
  if(adc_initialized != 2) {

    reg_wr(ADC_SMPR, 7);
    set_channel_mask(1 << channel);

    reg_wr(ADC_CR, (1 << 28) | (1 << 0) | (1 << 2));

    while(!(reg_rd(ADC_ISR) & 0x4)) {}
    reg_wr(ADC_ISR, 4);
    r = reg_rd(ADC_DR);
  } else {
    r = 0;
  }
  irq_permit(q);
  return r;
}



int
stm32g0_adc_vref(void)
{
  uint16_t vrefint = *(uint16_t *)0x1fff75aa;
  uint16_t v = adc_read_channel(13);
  return vrefint * 3000 / v;
}


static int
cmd_vref(cli_t *cli, int argc, char **argv)
{
  cli_printf(cli, "vref: %dmV\n", stm32g0_adc_vref());
  return 0;
}

CLI_CMD_DEF("vref", cmd_vref);


void
stm32g0_adc_multi(uint32_t channels,
                  uint16_t *output,
                  uint8_t trig)
{
  int q = irq_forbid(IRQ_LEVEL_SWITCH);

  int num_channels = __builtin_popcount(channels);

  adc_init(2);
  reg_wr(ADC_SMPR, 7);
  set_channel_mask(channels);

  reg_wr(ADC_CFGR1,
         (0b01 << 10) | // Trig on rising edge
         (trig << 6) |
         (1 << 1) | // DMACFG (Circular mode)
         (1 << 0) | // DMAEN
         0);

  reg_wr(ADC_CR, (1 << 28) | (1 << 0) | (1 << 2));

  stm32_dma_instance_t dmainst = stm32_dma_alloc(5, "adc");

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

  stm32_dma_set_paddr(dmainst, ADC_DR);
  stm32_dma_set_mem0(dmainst, output);
  stm32_dma_set_nitems(dmainst, num_channels);
  stm32_dma_start(dmainst);

  irq_permit(q);
}
