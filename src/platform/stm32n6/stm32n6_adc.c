#include "stm32n6_adc.h"
#include "stm32n6_clk.h"
#include "stm32n6_pwr.h"

#include <mios/io.h>
#include <mios/mios.h>
#include <mios/task.h>

#include <assert.h>
#include <stdio.h>
#include <unistd.h>

#include "irq.h"

// ---------------------------------------------------------------
// Kernel clock for ADC1/ADC2
//
// RCC_CCIPR1 ADC12SEL[2:0] (bits 6:4) selects the source:
//   000: hclk1 (= AHB1 clock, 200 MHz on this platform)
//   001: per_ck
//   010: ic7_ck     (not enabled on cecg2)
//   011: ic8_ck     (not enabled on cecg2)
//   100: msi_ck
//   101: hsi_div_ck
//   110: I2S_CKIN
//   111: timg_ck
//
// We pick hclk1 and divide by 4 via CCIPR1 ADCPRE[7:0] (bits 15:8;
// "v: ck_ker_adc12 / (v+1)") to land at 50 MHz — well below the
// 125 MHz spec max, with no dependence on extra IC clocks.
// ---------------------------------------------------------------

#define ADC12SEL_HCLK1 0b000
#define ADC12SEL_SHIFT 4

#define ADCPRE_DIV4    3
#define ADCPRE_SHIFT   8

static void
init_adc12_clk(void)
{
  if(clk_is_enabled(CLK_ADC12))
    return;

  reg_set_bits(RCC_CCIPR1, ADC12SEL_SHIFT, 3, ADC12SEL_HCLK1);
  reg_set_bits(RCC_CCIPR1, ADCPRE_SHIFT, 8, ADCPRE_DIV4);

  clk_enable(CLK_ADC12);

  // Pulse the peripheral reset to ensure clean state — BootROM
  // and earlier secure boot stages may have left things in some
  // partial configuration.
  rst_assert(CLK_ADC12);
  udelay(1);
  rst_deassert(CLK_ADC12);

  udelay(1);
}

// ---------------------------------------------------------------
// Per-instance bring-up
//
// Mirrors stm32h7_adc_init() step for step. The N6 ADC RM marks
// several CR/ISR bits as "reserved" that H7 documents as
// ADVREGEN / LDORDY / BOOST — but the silicon behaves like H7's,
// not like the RM literally describes (verified empirically: the
// minimal "just clear DEEPPWD + ADEN" sequence reads constant
// garbage on every channel because the analog frontend never
// fully comes up). So we drive the same boot flow as H7 ADC1/2.
//
//   pcsel — bitmap of channels to mark in the I/O preselection
//           register. Bit i must be set for any channel i used
//           in the regular sequence; for differential channel i
//           both bit i and bit i-1 must be set (RM0486 §32.4.12).
//           Pass 0 to disable everything (ADC will read garbage).
//
//   difsel — bitmap of channels to put into differential mode.
//            Pass 0 for all-single-ended.
// ---------------------------------------------------------------

void
stm32n6_adc_init(uint32_t base, uint32_t pcsel, uint32_t difsel)
{
  const char *name;
  switch(base) {
  case ADC1_BASE: name = "adc1"; break;
  case ADC2_BASE: name = "adc2"; break;
  default: panic("Unsupported ADC");
  }

  // VDDA18ADC supply must be marked valid in PWR_SVMCR3.ASV before
  // the ADC will produce non-zero samples. RM0486 §13.6 calls this
  // out as mandatory; without it the ADC reads as a stuck zero.
  stm32n6_pwr_vdda18adc_enable();

  // VREFBUF peripheral clock must be enabled before any access to
  // its registers, even just to read or to configure VREF+ in
  // external-reference mode.
  clk_enable(CLK_VREFBUF);

  // The N6 BootROM leaves VREFBUF_CSR at 0x0000 (ENVR=0, HIZ=0),
  // which per RM0486 Table 272 is "VREFBUF buffer off, VREF+ pin
  // pulled-down to VSSA". On boards that drive VREF+ externally
  // (e.g. tied to 3.3 V) this internal pull-down fights the
  // external supply — the ADC's effective reference ends up
  // somewhere indeterminate and every channel converts to a
  // single stuck mid-rail value. Set HIZ=1 to put the pin in
  // input mode (high-Z) so the external reference drives it
  // cleanly. Likely the same fix referenced by errata 2.2.17
  // (VREFP pull-down patch).
  reg_wr(0x56003C00, 0x0002);

  init_adc12_clk();

  // CR bit 29 = DEEPPWD, reset state = 1. If already cleared, the
  // ADC has been brought up before — bail.
  if(reg_get_bit(base + ADCx_CR, 29) == 0)
    return;

  reg_clr_bit(base + ADCx_CR, 29);  // Exit DEEPPWD

  // Per RM0486, clearing DEEPPWD is the only software step before
  // ADEN — there is no separate ADVREGEN bit (CR bits 28:6 are
  // marked reserved on N6). Fixed delay covers tSTAB.
  udelay(20);

  // BOOST = 0b11 in CR bits 9:8 (matches H7). The N6 RM marks
  // bits 28:6 as reserved, but the silicon appears to track H7's
  // layout — without BOOST, the input multiplexer reads back a
  // stuck value regardless of channel selection.
  reg_set_bits(base + ADCx_CR, 8, 2, 0b11);

  reg_wr(base + ADCx_DIFSEL, difsel);
  reg_wr(base + ADCx_PCSEL, pcsel);

  reg_set_bit(base + ADCx_CR, 0);  // ADEN
  while(reg_get_bit(base + ADCx_ISR, 0) == 0) {}

  // Mandatory single-ended calibration per RM0486 §32.4.8. Without
  // it the input multiplexer doesn't behave correctly (every
  // channel reads back a constant ~500 LSB). The procedure runs
  // *with* the ADC enabled: set ADCAL, perform 8 conversions of
  // the internal calibration source, average, write the result
  // into CALFACT_S, then clear ADCAL.
  reg_clr_bit(base + ADCx_CR, 30);            // ADCALDIF = 0
  reg_clr_bit(base + ADCx_CALFACT, 31);       // CALADDOS = 0
  reg_set_bit(base + ADCx_CR, 31);            // ADCAL = 1
  for(int retry = 0; retry < 2; retry++) {
    uint32_t sum = 0;
    for(int i = 0; i < 8; i++) {
      reg_set_bit(base + ADCx_CR, ADCx_CR_ADSTART_Pos);
      while(!reg_get_bit(base + ADCx_ISR, ADCx_ISR_EOC_Pos)) {}
      sum += reg_rd(base + ADCx_DR) & 0xfff;
    }
    uint32_t avg = sum / 8;
    if(avg != 0) {
      reg_set_bits(base + ADCx_CALFACT, 0, 9, avg);
      break;
    }
    reg_set_bit(base + ADCx_CALFACT, 31);     // CALADDOS = 1, retry
  }
  reg_clr_bit(base + ADCx_CR, 31);            // ADCAL = 0

  printf("%s: ADC ready\n", name);
}

void
stm32n6_adc_set_smpr(uint32_t base, uint32_t channel, uint32_t value)
{
  if(channel < 10) {
    reg_set_bits(base + ADCx_SMPR1, channel * 3, 3, value);
  } else {
    reg_set_bits(base + ADCx_SMPR2, (channel - 10) * 3, 3, value);
  }
}

// SQR field layout (RM0486 §32.6.9-12):
//   SQR1: L[3:0] | SQ1 | SQ2 | SQ3 | SQ4    (4 ranks)
//   SQR2:        | SQ5 | … | SQ9            (5 ranks)
//   SQR3:        | SQ10 | … | SQ14          (5 ranks)
//   SQR4:        | SQ15 | SQ16              (2 ranks)
// Each SQ field is 5 bits at a 6-bit stride starting at bit 6 in
// SQR1 and bit 0 in SQR2/3/4. Treating L as the "rank-0 slot" of
// SQR1 lets the same (seq/5, seq%5) decomposition cover all four
// registers — the only N6-vs-H7 difference is the field width
// (5 instead of 6).
void
stm32n6_adc_set_seq_channel(uint32_t base, uint32_t seq, uint32_t channel)
{
  const uint32_t reg = seq / 5;
  const uint32_t bo = seq % 5;
  reg_set_bits(base + ADCx_SQRx(reg), bo * 6, 5, channel);
}

void
stm32n6_adc_set_seq_length(uint32_t base, uint32_t length)
{
  reg_set_bits(base + ADCx_SQRx(0), 0, 4, length - 1);
}

// Configure the injected sequence to convert one channel under
// software trigger. JSQR layout (RM0486 §32.6.14):
//   bits 1:0   = JL[1:0]   (length-1; 0 = 1 conversion)
//   bits 6:2   = JEXTSEL   (ignored when JEXTEN=0)
//   bits 8:7   = JEXTEN    (00 = software-only)
//   bits 13:9  = JSQ1[4:0] (channel for rank 1)
void
stm32n6_adc_set_injected_channel(uint32_t base, int channel)
{
  reg_wr(base + ADCx_JSQR, channel << 9);
}

uint32_t
stm32n6_adc_set_channels(uint32_t base, uint32_t channels)
{
  int seq = 0;
  for(int i = 0; i < 32; i++) {
    if((1 << i) & channels) {
      stm32n6_adc_set_seq_channel(base, seq + 1, i);
      seq++;
    }
  }
  assert(seq > 0);
  stm32n6_adc_set_seq_length(base, seq);
  return seq;
}

// ---------------------------------------------------------------
// Blocking single-channel read (diagnostic / CLI use)
// ---------------------------------------------------------------

static mutex_t adc1_mtx = MUTEX_INITIALIZER("adc1");
static mutex_t adc2_mtx = MUTEX_INITIALIZER("adc2");

static mutex_t *
mtx_for(uint32_t base)
{
  switch(base) {
  case ADC1_BASE: return &adc1_mtx;
  case ADC2_BASE: return &adc2_mtx;
  default: panic("Unsupported ADC");
  }
}

int
stm32n6_adc_read_channel(uint32_t base, int channel)
{
  mutex_t *mtx = mtx_for(base);

  mutex_lock(mtx);

  // Sequence length 1, rank 1 → channel. SQR1 layout: L[3:0] at
  // bits 3:0, SQ1[4:0] at bits 10:6.
  reg_wr(base + ADCx_SQRx(0), channel << 6);

  reg_set_bit(base + ADCx_CR, ADCx_CR_ADSTART_Pos);
  while(reg_get_bit(base + ADCx_ISR, ADCx_ISR_EOC_Pos) == 0) {}

  int ret = reg_rd(base + ADCx_DR);

  mutex_unlock(mtx);
  return ret;
}

// ---------------------------------------------------------------
// DMA path — programs the channel sequence and wires up an HPDMA1
// channel peripheral→memory. Mirrors stm32g4_adc_init_dma().
// ---------------------------------------------------------------

stm32_dma_instance_t
stm32n6_adc_init_dma(uint32_t base, uint32_t channels)
{
  uint32_t seqlen = stm32n6_adc_set_channels(base, channels);

  stm32_dma_instance_t dmainst;
  switch(base) {
  case ADC1_BASE:
    dmainst = stm32_dma_alloc(STM32N6_DMA_ADC1, "adc1");
    break;
  case ADC2_BASE:
    dmainst = stm32_dma_alloc(STM32N6_DMA_ADC2, "adc2");
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

  // CFGR1 DMNGT[1:0] (bits 1:0) = 11 → DMA-circular mode.
  reg_set_bits(base + ADCx_CFGR1, 0, 2, 0b11);
  return dmainst;
}

error_t
stm32n6_adc_dma_transfer(uint32_t base, stm32_dma_instance_t dmainst,
                         uint16_t *output)
{
  stm32_dma_set_mem0(dmainst, output);
  int q = irq_forbid(IRQ_LEVEL_SCHED);
  stm32_dma_start(dmainst);
  reg_set_bit(base + ADCx_CR, ADCx_CR_ADSTART_Pos);
  error_t error = stm32_dma_wait(dmainst);
  irq_permit(q);
  return error;
}
