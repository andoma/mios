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
#include "stm32g0_adc.h"

static mutex_t adc_mutex = MUTEX_INITIALIZER("adc");
static uint8_t adc_initialized;

static void
adc_init(void)
{
  if(adc_initialized)
    return;
  adc_initialized = 1;

  clk_enable(CLK_ADC);

  // Enable Voltage Regulator
  reg_wr(ADC_CR, (1 << 28));
  usleep(20);

  // Enable VREFEN
  reg_set_bit(ADC_CCR, 22);

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


int
adc_read_channel(int channel)
{
  mutex_lock(&adc_mutex);

  adc_init();

  reg_wr(ADC_CHSELR, (1 << channel));
  reg_wr(ADC_SMPR, 7);
  while(!(reg_rd(ADC_ISR) & (1 << 13))) {}
  reg_wr(ADC_ISR, (1 << 13));

  reg_wr(ADC_CR, (1 << 28) | (1 << 0) | (1 << 2));

  while(!(reg_rd(ADC_ISR) & 0x4)) {}
  reg_wr(ADC_ISR, 4);
  int r = reg_rd(ADC_DR);
  mutex_unlock(&adc_mutex);
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
  printf("vref: %dmV\n", stm32g0_adc_vref());
  return 0;
}

CLI_CMD_DEF("vref", cmd_vref);
