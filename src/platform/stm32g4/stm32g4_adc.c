#include "stm32g4_adc.h"
#include "stm32g4_clk.h"
#include "stm32g4_dma.h"

#include <mios/io.h>
#include <mios/mios.h>

#include <assert.h>
#include <unistd.h>
#include <stdio.h>

#include "irq.h"


static void
init_adc12(void)
{
  static uint8_t initialized;
  if(initialized)
    return;
  initialized = 1;

  reg_set_bits(RCC_CCIPR, 28, 2, 1); // select PLLP as ADC12 CLK
  clk_enable(CLK_ADC12);
}




void
stm32g4_adc_init(uint32_t base)
{
  switch(base) {
  case ADC1_BASE:
  case ADC2_BASE:
    init_adc12();
    break;
  default:
    panic("Unsupported ADC");
  }

  if(reg_get_bit(base + ADCx_CR, 29) == 0)
    return; // Already initialized

  reg_clr_bit(base + ADCx_CR, 29); // Turn off DEEPPWD
  reg_set_bit(base + ADCx_CR, 28);  // Turn on voltage regulator

  udelay(200); // Wait for voltage regulator to stabilize

  // Enable self-calibration, and wait for it to complete
  reg_set_bit(base + ADCx_CR, 31);
  while(1) {
    if(!reg_get_bit(base + ADCx_CR, 31))
      break;
  }

  udelay(5);

  reg_set_bit(base + ADCx_CR, 0); // Turn on ADEN

  while(1) {
    if(reg_get_bit(base + ADCx_ISR, 0))
      break;
  }
}


void
stm32g4_adc_set_smpr(uint32_t base, uint32_t channel, uint32_t value)
{
  if(channel < 10) {
    reg_set_bits(base + ADCx_SMPR1, channel * 3, 3, value);
  } else {
    reg_set_bits(base + ADCx_SMPR2, (channel - 10) * 3, 3, value);
  }
}


void
stm32g4_set_seq_channel(uint32_t base, uint32_t seq, uint32_t channel)
{
  const uint32_t reg = seq / 5;
  const uint32_t bo = seq % 5;
  reg_set_bits(base + ADCx_SQRx(reg), bo * 6, 6, channel);
}

void
stm32g4_set_seq_length(uint32_t base, uint32_t length)
{
  reg_set_bits(base + ADCx_SQRx(0), 0, 4, length - 1);
}

uint32_t
stm32g4_adc_set_channels(uint32_t base, uint32_t channels)
{
  int seq = 0;
  for(int i = 0; i < 32; i++) {
    if((1 << i) & channels) {
      stm32g4_set_seq_channel(base, seq + 1, i);
      seq++;
    }
  }
  assert(seq > 0);
  stm32g4_set_seq_length(base, seq);
  return seq;
}

stm32_dma_instance_t
stm32g4_adc_init_dma(uint32_t base, uint32_t channels)
{
  uint32_t seqlen = stm32g4_adc_set_channels(base, channels);

  stm32_dma_instance_t dmainst;
  switch(base) {
  case ADC1_BASE:
    dmainst = stm32_dma_alloc(5, "adc1");
    break;
  case ADC2_BASE:
    dmainst = stm32_dma_alloc(36, "adc2");
    break;
  default:
    panic("Unsupported ADC");
  }

  stm32_dma_make_waitable(dmainst, "adc");

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

  stm32_dma_set_paddr(dmainst, base + ADCx_DR);

  reg_set_bit(base + ADCx_CFGR, 0); // DMA mode
  return dmainst;
}


error_t
stm32g4_adc_dma_transfer(uint32_t base, uint32_t dmainst, uint16_t *output)
{
  stm32_dma_set_mem0(dmainst, output);
  int q = irq_forbid(IRQ_LEVEL_SCHED);
  stm32_dma_start(dmainst);
  reg_set_bit(base + ADCx_CR, 2);
  error_t error = stm32_dma_wait(dmainst);
  irq_permit(q);
  return error;
}




#if 0

#include <mios/cli.h>


static int
adc_read_channel(int channel)
{
  stm32g4_adc12_init();

  reg_wr(ADC1_BASE + ADCx_SMPR2, 1 << 24);
  reg_wr(ADC1_BASE + ADCx_SQR1, channel << 6);
  reg_set_bit(ADC1_BASE + ADCx_CR, 2);

  int64_t ts = clock_get();
  while(1) {
    if(reg_get_bit(ADC1_BASE + ADCx_ISR, 2))
      break;
  }
  int r =  reg_rd(ADC1_BASE + ADCx_DR);
  ts = clock_get() - ts;
  printf("Readout took %dµs\n", (int)ts);
  return r;
}





int
stm32g4_adc_vref(void)
{
  uint16_t vrefint = *(uint16_t *)0x1fff75aa;
  uint16_t v = adc_read_channel(18);
  return vrefint * 3000 / v;
}


static error_t
cmd_vref(cli_t *cli, int argc, char **argv)
{
  cli_printf(cli, "vref: %dmV\n", stm32g4_adc_vref());
  return 0;
}

CLI_CMD_DEF("vref", cmd_vref);





#include "stm32g4_opamp.h"

static error_t
cmd_adc(cli_t *cli, int argc, char **argv)
{
  clk_enable(CLK_SYSCFG);
  stm32g4_adc12_init();

  reg_wr(OPAMP_CSR(1),
         0b10 << 5 |    // VM_SEL = PGA
         (1 << 8) |     // Internal output enable (to ADC)
         (0b11 << 14) | // GAIN = x16
         0);
  reg_set_bit(OPAMP_CSR(1), 0); // Enable OPAMP

  reg_wr(ADC1_BASE + ADCx_SMPR2, 7 << 9); // Channel 13
  reg_wr(ADC1_BASE + ADCx_SQR1, 13 << 6);

  reg_set_bit(ADC1_BASE + ADCx_CFGR, 13);
  reg_set_bit(ADC1_BASE + ADCx_CFGR, 12);
  reg_set_bit(ADC1_BASE + ADCx_CR, 2);
#if 0
  uint16_t vrefint = *(uint16_t *)0x1fff75aa;
  while(1) {
    printf("%d\n", vrefint * 3000 / reg_rd(ADC1_BASE + ADCx_DR));
  }
#endif
  while(1) {
    int v = reg_rd(ADC1_BASE + ADCx_DR);
    v -= 2003;
    float I = v * 0.01f;
    printf("A=%f\n", I);
  }

  return 0;
}

CLI_CMD_DEF("adc", cmd_adc);

#endif
