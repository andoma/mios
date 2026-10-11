#include "stm32n6_csi.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <mios/mios.h>
#include <mios/eventlog.h>
#include <mios/type_macros.h>

#include "irq.h"
#include "stm32n6_clk.h"
#include "stm32n6_csi_reg.h"

// The D-PHY configuration clock (ic18_ck) must be within 17..27 MHz and
// is communicated to the PHY via PFCR.CCFR. 24 MHz also puts the PHY's
// cfgclkfreqrange inside the window the 0xE4 test code value assumes.
#define CSI_CFG_CLK_HZ 24000000

// CSI processing clock is the DCMIPP kernel clock (max 333 MHz)
#define DCMIPP_KER_CLK_HZ 300000000

static const struct {
  uint16_t mbps;
  uint8_t hsfr;
  uint16_t osc_freq;
} dphy_freqs[] = {
#define X(mbps, hsfr, osc) { mbps, hsfr, osc },
  CSI_DPHY_FREQ_TABLE(X)
#undef X
};

static struct {
  uint32_t sof[4];
  uint32_t eof[4];
  uint32_t errors;
  uint32_t crc_errors;
  uint32_t sr0_err_accum;
  uint32_t sr1_err_accum;
  uint32_t last_err1;
  uint32_t last_err2;
  uint16_t mbps;
  uint8_t lanes;
  uint8_t irq_enabled;
} csi;


static void
csi_irq(void *arg)
{
  const uint32_t sr0 = reg_rd(CSI_SR0) & reg_rd(CSI_IER0);
  const uint32_t sr1 = reg_rd(CSI_SR1) & CSI_SR1_ERRMASK;

  reg_wr(CSI_FCR0, sr0 & CSI_FCR0_ALL);
  reg_wr(CSI_FCR1, sr1);

  for(int vc = 0; vc < 4; vc++) {
    if(sr0 & CSI_SR0_SOFxF(vc))
      csi.sof[vc]++;
    if(sr0 & CSI_SR0_EOFxF(vc))
      csi.eof[vc]++;
  }

  if(sr0 & CSI_SR0_CCFIFOFF) {
    // Processing clock can't keep up. It will keep firing, so report it
    // once and mask it (as the ST HAL does)
    reg_clr_bit(CSI_IER0, 21);
  }

  const uint32_t err0 = sr0 & CSI_SR0_ERRMASK;
  if(err0 & CSI_SR0_CRCERRF)
    csi.crc_errors++;
  if(err0 || sr1) {
    csi.errors++;
    csi.sr0_err_accum |= err0;
    csi.sr1_err_accum |= sr1;
    csi.last_err1 = reg_rd(CSI_ERR1);
    csi.last_err2 = reg_rd(CSI_ERR2);
  }
}


// Write an 8 bit value to a Synopsys D-PHY RX test code register, using
// the testen/testclk/testdin protocol on PTCR0/PTCR1 (same sequence as
// the ST HAL, the test code MSB is always zero here)
static void
dphy_test_write(uint8_t code, uint8_t value)
{
  // Test code MSB phase
  reg_wr(CSI_PTCR1, CSI_PTCR1_TWM);
  reg_wr(CSI_PTCR0, CSI_PTCR0_TCKEN);
  reg_wr(CSI_PTCR0, 0);
  reg_wr(CSI_PTCR1, 0);
  reg_wr(CSI_PTCR0, CSI_PTCR0_TCKEN);

  // Test code LSB phase
  reg_wr(CSI_PTCR0, 0);
  reg_wr(CSI_PTCR1, CSI_PTCR1_TWM);
  reg_wr(CSI_PTCR0, CSI_PTCR0_TCKEN);
  reg_wr(CSI_PTCR1, CSI_PTCR1_TWM | code);
  reg_wr(CSI_PTCR0, 0);
  reg_wr(CSI_PTCR1, 0);

  // Data phase
  reg_wr(CSI_PTCR1, value);
  reg_wr(CSI_PTCR0, CSI_PTCR0_TCKEN);
  reg_wr(CSI_PTCR0, 0);
}


static void
csi_clear_stats(void)
{
  memset(csi.sof, 0, sizeof(csi.sof));
  memset(csi.eof, 0, sizeof(csi.eof));
  csi.errors = 0;
  csi.crc_errors = 0;
  csi.sr0_err_accum = 0;
  csi.sr1_err_accum = 0;
  csi.last_err1 = 0;
  csi.last_err2 = 0;
}


static int
wait_bits(uint32_t addr, uint32_t mask, uint32_t want, int timeout_us)
{
  for(int i = 0; i < timeout_us; i += 10) {
    if((reg_rd(addr) & mask) == want)
      return 0;
    udelay(10);
  }
  return -1;
}


error_t
stm32n6_csi_init(int lanes, int mbps)
{
  if(lanes < 1 || lanes > 2)
    return ERR_INVALID_ARGS;

  // Pick the D-PHY band closest to the requested lane rate
  size_t best = 0;
  for(size_t i = 1; i < ARRAYSIZE(dphy_freqs); i++) {
    if(abs(dphy_freqs[i].mbps - mbps) < abs(dphy_freqs[best].mbps - mbps))
      best = i;
  }
  const uint8_t hsfr = dphy_freqs[best].hsfr;
  const uint16_t osc_freq = dphy_freqs[best].osc_freq;
  csi.mbps = dphy_freqs[best].mbps;
  csi.lanes = lanes;
  csi_clear_stats();

  // DCMIPP kernel clock, which is also the CSI processing clock
  stm32n6_ic_enable_pll1(17, DCMIPP_KER_CLK_HZ);
  reg_set_bits(RCC_CCIPR1, 20, 2, 2); // DCMIPPSEL = ic17_ck

  // D-PHY configuration and escape clock
  const unsigned int cfg_clk = stm32n6_ic_enable_pll1(18, CSI_CFG_CLK_HZ);
  // cfgclkfreqrange = (Fcfg[MHz] - 17) * 4
  const uint32_t ccfr = (cfg_clk / 250000) - 17 * 4;

  clk_enable(CLK_CSI);
  reg_set_bit(RCC_APB5LPENR, 6); // Keep it clocked during WFI
  rst_assert(CLK_CSI);
  rst_deassert(CLK_CSI);

  // Controller static configuration (lane merger only while disabled)
  reg_wr(CSI_CR, 0);
  if(lanes == 2) {
    reg_wr(CSI_LMCFGR, CSI_LMCFGR_2LANE_STRAIGHT);
  } else {
    reg_wr(CSI_LMCFGR, (1 << CSI_LMCFGR_LANENB_SHIFT) |
           (1 << CSI_LMCFGR_DL0MAP_SHIFT));
  }
  reg_wr(CSI_WDR, 0);
  reg_wr(CSI_CR, CSI_CR_CSIEN);

  reg_wr(CSI_FCR0, CSI_FCR0_ALL);
  reg_wr(CSI_FCR1, CSI_FCR1_ALL);
  reg_wr(CSI_IER0,
         CSI_IER0_CCFIFOFIE | CSI_IER0_SYNCERRIE | CSI_IER0_SPKTERRIE |
         CSI_IER0_CRCERRIE | CSI_IER0_ECCERRIE | CSI_IER0_WDERRIE);
  reg_wr(CSI_IER1, lanes == 2 ? CSI_IER1_ALL : 0x1f);

  // D-PHY bring-up: hold the PHY in reset with all lanes off
  reg_wr(CSI_PRCR, 0);
  reg_wr(CSI_PCR, 0);

  // Reset the test interface (the HAL pulses testclk, the Synopsys
  // procedure uses testclr, so do both)
  reg_wr(CSI_PTCR0, CSI_PTCR0_TCKEN | CSI_PTCR0_TRSEN);
  udelay(1000);
  reg_wr(CSI_PTCR0, 0);

  reg_wr(CSI_PFCR, (hsfr << CSI_PFCR_HSFR_SHIFT) | ccfr);

  dphy_test_write(DPHY_TC_DESKEW_POL, 0x38);
  dphy_test_write(DPHY_TC_DES_EN_CNT, 0x11);
  dphy_test_write(DPHY_TC_OSC_FREQ_HI, osc_freq >> 8);
  dphy_test_write(DPHY_TC_OSC_FREQ_LO, osc_freq & 0xff);

  // DLD and PWRDOWN polarities follow the ST HAL (the RM text has them
  // the other way around)
  reg_wr(CSI_PFCR, (hsfr << CSI_PFCR_HSFR_SHIFT) | ccfr | CSI_PFCR_DLD);

  uint32_t pcr = CSI_PCR_CLEN | CSI_PCR_DL0EN | CSI_PCR_PWRDOWN;
  if(lanes == 2)
    pcr |= CSI_PCR_DL1EN;
  reg_wr(CSI_PCR, pcr);

  reg_wr(CSI_PRCR, CSI_PRCR_PEN);
  reg_wr(CSI_PMCR, 0);

  if(!csi.irq_enabled) {
    irq_enable_fn_arg(47, IRQ_LEVEL_IO, csi_irq, NULL);
    csi.irq_enabled = 1;
  }

  // Lanes should settle in LP-11 (stop state) if the sensor is powered.
  // Some sensors only drive the lanes once streaming, so just report it.
  uint32_t stop = CSI_SR1_STOPCLF | CSI_SR1_STOPDL0F;
  if(lanes == 2)
    stop |= CSI_SR1_STOPDL1F;
  if(wait_bits(CSI_SR1, stop, stop, 5000)) {
    evlog(LOG_WARNING, "csi: lanes not in stop state (SR1=0x%08x)",
          reg_rd(CSI_SR1));
  }

  evlog(LOG_INFO, "csi: %d lane%s @ %d Mbps (hsfr 0x%02x, cfgclk %d kHz)",
        lanes, lanes > 1 ? "s" : "", csi.mbps, hsfr, cfg_clk / 1000);
  return 0;
}


void
stm32n6_csi_config_vc(int vc, int dt, int ft)
{
  reg_wr(CSI_VCxCFGR1(vc),
         (ft << CSI_VCCFGR1_DT0FT_SHIFT) |
         (dt << CSI_VCCFGR1_DT0_SHIFT) |
         CSI_VCCFGR1_DTyEN(0));
}


error_t
stm32n6_csi_start(int vc)
{
  if(reg_rd(CSI_SR0) & CSI_SR0_VCxSTATEF(vc))
    return ERR_BAD_STATE;

  reg_wr(CSI_CR, CSI_CR_CSIEN | CSI_CR_VCxSTART(vc));
  reg_or(CSI_IER0, CSI_IER0_SOFxIE(vc) | CSI_IER0_EOFxIE(vc));
  return 0;
}


void
stm32n6_csi_stop(int vc)
{
  reg_wr(CSI_CR, CSI_CR_CSIEN | CSI_CR_VCxSTOP(vc));
  // The virtual channel stops at the next end of frame
  if(wait_bits(CSI_SR0, CSI_SR0_VCxSTATEF(vc), 0, 200000))
    evlog(LOG_WARNING, "csi: VC%d did not stop", vc);
  reg_wr(CSI_IER0, reg_rd(CSI_IER0) &
         ~(CSI_IER0_SOFxIE(vc) | CSI_IER0_EOFxIE(vc)));
}


unsigned int
stm32n6_csi_frame_count(int vc)
{
  return csi.eof[vc];
}


void
stm32n6_csi_print_status(stream_t *st)
{
  if(!clk_is_enabled(CLK_CSI)) {
    stprintf(st, "  Not initialized\n");
    return;
  }
  stprintf(st, "  %d lane%s, D-PHY band %d Mbps  SR0:0x%08x SR1:0x%08x\n",
           csi.lanes, csi.lanes > 1 ? "s" : "", csi.mbps,
           reg_rd(CSI_SR0), reg_rd(CSI_SR1));
  for(int vc = 0; vc < 4; vc++) {
    if(csi.sof[vc] || csi.eof[vc])
      stprintf(st, "  VC%d: SOF:%u EOF:%u\n", vc, csi.sof[vc], csi.eof[vc]);
  }
  stprintf(st, "  Errors:%u (CRC:%u)  Seen SR0:0x%08x SR1:0x%08x  "
           "ERR1:0x%08x ERR2:0x%08x\n",
           csi.errors, csi.crc_errors, csi.sr0_err_accum, csi.sr1_err_accum,
           csi.last_err1, csi.last_err2);
}


void
stm32n6_csi_clear_stats(void)
{
  csi_clear_stats();
}
