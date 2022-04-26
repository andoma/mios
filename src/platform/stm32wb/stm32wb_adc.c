#include <mios/cli.h>
#include <unistd.h>

#include "stm32wb_adc.h"
#include "stm32wb_clk.h"
#include "irq.h"

static uint8_t adc_initialized;

static void
adc_init(int how)
{
  if(adc_initialized)
    return;
  adc_initialized = how;

  reg_set_bits(RCC_CCIPR, 28, 2, 3);

  clk_enable(CLK_ADC);

  // Clear DEEPPWD
  reg_wr(ADC_CR, 0);
  udelay(20);
  // Enable Voltage Regulator (ADVREGEN)
  reg_wr(ADC_CR, (1 << 28));
  udelay(20);

  int presc = 0;
  // Enable VREFEN
  reg_wr(ADC_CCR,
         (1 << 24) |
         (1 << 22) |
         (presc << 18));
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
stm32wb_adc_set_smpr(int channel, int value)
{
  if(channel < 10) {
    reg_set_bits(ADC_SMPR1, channel * 3, 3, value);
  } else {
    reg_set_bits(ADC_SMPR2, (channel - 10) * 3, 3, value);
  }
}



int
adc_read_channel(int channel)
{
  int q = irq_forbid(IRQ_LEVEL_SWITCH);
  adc_init(1);

  int r;
  if(adc_initialized != 2) {

    stm32wb_adc_set_smpr(channel, 7);

    reg_wr(ADC_SQR1, channel << 6);

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
stm32wb_adc_vref(void)
{
  uint16_t vrefint = *(uint16_t *)0x1fff75aa;
  uint16_t v = adc_read_channel(0);
  return vrefint * 3600 / v;
}


static error_t
cmd_vref(cli_t *cli, int argc, char **argv)
{
  cli_printf(cli, "vref: %dmV\n", stm32wb_adc_vref());
  return 0;
}

CLI_CMD_DEF("vref", cmd_vref);


static error_t
cmd_vbat(cli_t *cli, int argc, char **argv)
{
  int ch18 = adc_read_channel(18);
  int vbat = 3 * ((ch18 * stm32wb_adc_vref()) >> 12);
  cli_printf(cli, "vbat: %dmV\n", vbat);
  return 0;
}

CLI_CMD_DEF("vbat", cmd_vbat);
