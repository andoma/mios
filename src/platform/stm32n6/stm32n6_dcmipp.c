#include "stm32n6_dcmipp.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <malloc.h>

#include <mios/task.h>
#include <mios/eventlog.h>

#include "cache.h"
#include "irq.h"
#include "stm32n6_clk.h"
#include "stm32n6_rif.h"
#include "stm32n6_dcmipp_reg.h"

#define DCMIPP_IRQ 48

#define RIF_MASTER_DCMIPP     9
#define RIF_PERIPH_DCMIPP_REG 2
#define RIF_PERIPH_DCMIPP_BIT 29

#define DCMIPP_KER_CLK_HZ 300000000

static struct {
  task_waitable_t wait;

  // Pipe0 snapshot
  uint8_t p0_done;

  // Pipe1
  uint8_t p1_running;
  uint8_t p1_ring;
  uint32_t p1_seq;
  uint32_t p1_line_events; // Since last end of frame
  void *p1_last;
  uint16_t stat_width;
  uint16_t stat_height;
  stm32n6_dcmipp_stats_t stats;

  uint32_t vsync;
  uint32_t overruns;
  uint32_t axi_errors;

  uint8_t initialized;
} dcmipp;


static void
dcmipp_irq(void *arg)
{
  const uint32_t sr = reg_rd(DCMIPP_CMSR2) & reg_rd(DCMIPP_CMIER);
  reg_wr(DCMIPP_CMFCR, sr);

  if(sr & DCMIPP_CM_P1VSYNC)
    dcmipp.vsync++;

  // A line event is also raised at the end of frame, so do this first
  if(sr & DCMIPP_CM_P1LINE)
    dcmipp.p1_line_events++;

  if(sr & DCMIPP_CM_P1FRAME) {
    dcmipp.p1_line_events = 0;
    dcmipp.p1_last = (void *)reg_rd(DCMIPP_P1STM0AR);
    dcmipp.p1_seq++;

    // Statistics are latched at frame end and stay stable for the whole
    // next frame. "Pre" sources only see their own Bayer sites.
    const uint32_t n = dcmipp.stat_width * dcmipp.stat_height;
    if(n) {
      dcmipp.stats.r = (reg_rd(DCMIPP_P1ST1SR) & 0xffffff) * 256 / (n / 4);
      dcmipp.stats.g = (reg_rd(DCMIPP_P1ST2SR) & 0xffffff) * 256 / (n / 2);
      dcmipp.stats.b = (reg_rd(DCMIPP_P1ST3SR) & 0xffffff) * 256 / (n / 4);
      dcmipp.stats.seq = dcmipp.p1_seq;
    }
    task_wakeup(&dcmipp.wait, 1);
  }

  if(sr & DCMIPP_CM_P0FRAME) {
    dcmipp.p0_done = 1;
    task_wakeup(&dcmipp.wait, 1);
  }

  if(sr & (DCMIPP_CM_P1OVR | DCMIPP_CM_P0OVR))
    dcmipp.overruns++;

  if(sr & DCMIPP_CM_ATXERR)
    dcmipp.axi_errors++;
}


void
stm32n6_dcmipp_init(void)
{
  if(dcmipp.initialized)
    return;

  // Kernel clock (also the CSI processing clock)
  stm32n6_ic_enable_pll1(17, DCMIPP_KER_CLK_HZ);
  reg_set_bits(RCC_CCIPR1, 20, 2, 2); // DCMIPPSEL = ic17_ck

  clk_enable(CLK_DCMIPP);
  reg_set_bit(RCC_APB5LPENR, 2);
  rst_assert(CLK_DCMIPP);
  rst_deassert(CLK_DCMIPP);

  // Secure peripheral and secure AXI master, same as ETH1
  stm32n6_rif_set_periph_sec(RIF_PERIPH_DCMIPP_REG, RIF_PERIPH_DCMIPP_BIT, 1, 1);
  stm32n6_rif_set_master_attr(RIF_MASTER_DCMIPP, 1, 1, 1);

  reg_wr(DCMIPP_CMCR, DCMIPP_CMCR_INSEL); // Input from CSI-2

  task_waitable_init(&dcmipp.wait, "dcmipp");

  reg_wr(DCMIPP_CMFCR, 0xffffffff);
  reg_wr(DCMIPP_CMIER, DCMIPP_CM_ATXERR);
  irq_enable_fn_arg(DCMIPP_IRQ, IRQ_LEVEL_IO, dcmipp_irq, NULL);
  dcmipp.initialized = 1;
}


static void
pipe_stop(uint32_t fctcr, uint32_t fscr, uint32_t cptact_mask)
{
  reg_wr(fctcr, reg_rd(fctcr) & ~DCMIPP_FCTCR_CPTREQ);
  for(int i = 0; i < 100; i++) {
    if(!(reg_rd(DCMIPP_CMSR1) & cptact_mask))
      break;
    usleep(1000);
  }
  reg_wr(fscr, reg_rd(fscr) & ~DCMIPP_FSCR_PIPEN);
}


error_t
stm32n6_dcmipp_snapshot_raw(uint16_t *buf, int vc, int dt,
                            int x, int y, int width, int height)
{
  if((x & 1) || (width & 1) || ((intptr_t)buf & 15))
    return ERR_INVALID_ARGS;

  const size_t size = width * height * 2;

  // No dirty lines on top of what the DCMIPP writes
  dcache_op(buf, size, DCACHE_CLEAN | DCACHE_INVALIDATE);

  reg_wr(DCMIPP_P0FSCR,
         DCMIPP_FSCR_VC(vc) | DCMIPP_FSCR_DTMODE(DCMIPP_DTMODE_DTIDA) |
         DCMIPP_FSCR_DTIDA(dt));

  // Horizontal crop is in 32 bit words (two 16 bit pixels)
  reg_wr(DCMIPP_P0SCSTR,
         DCMIPP_P0SCSTR_VSTART(y) | DCMIPP_P0SCSTR_HSTART(x / 2));
  reg_wr(DCMIPP_P0SCSZR,
         DCMIPP_P0SCSZR_ENABLE |
         DCMIPP_P0SCSZR_VSIZE(height) | DCMIPP_P0SCSZR_HSIZE(width / 2));

  reg_wr(DCMIPP_P0PPCR, 0);
  reg_wr(DCMIPP_P0PPM0AR1, (intptr_t)buf);
  reg_wr(DCMIPP_P0DCLMTR, DCMIPP_P0DCLMTR_ENABLE |
         DCMIPP_P0DCLMTR_LIMIT(size / 4));

  const int q = irq_forbid(IRQ_LEVEL_IO);
  dcmipp.p0_done = 0;
  reg_wr(DCMIPP_CMFCR, DCMIPP_CM_P0FRAME | DCMIPP_CM_P0OVR |
         DCMIPP_CM_P0LIMIT | DCMIPP_CM_P0VSYNC);
  reg_or(DCMIPP_CMIER, DCMIPP_CM_P0FRAME | DCMIPP_CM_P0OVR);

  reg_wr(DCMIPP_P0FCTCR, DCMIPP_FCTCR_CPTMODE); // Snapshot
  reg_or(DCMIPP_P0FSCR, DCMIPP_FSCR_PIPEN);
  reg_or(DCMIPP_P0FCTCR, DCMIPP_FCTCR_CPTREQ);

  const int64_t deadline = clock_get() + 500000;
  error_t err = 0;
  while(!dcmipp.p0_done) {
    if(task_sleep_deadline(&dcmipp.wait, deadline)) {
      err = ERR_TIMEOUT;
      break;
    }
  }
  reg_wr(DCMIPP_CMIER, reg_rd(DCMIPP_CMIER) &
         ~(DCMIPP_CM_P0FRAME | DCMIPP_CM_P0OVR));
  irq_permit(q);

  pipe_stop(DCMIPP_P0FCTCR, DCMIPP_P0FSCR, DCMIPP_CMSR1_P0CPTACT);

  dcache_op(buf, size, DCACHE_INVALIDATE);
  return err;
}


size_t
stm32n6_dcmipp_uv_offset(const stm32n6_dcmipp_pipe1_config_t *cfg)
{
  if(cfg->ring_lines)
    return cfg->out_width * cfg->ring_lines * 2;
  return cfg->out_width * cfg->out_height;
}


size_t
stm32n6_dcmipp_buffer_size(const stm32n6_dcmipp_pipe1_config_t *cfg)
{
  return stm32n6_dcmipp_uv_offset(cfg) * 3 / 2;
}


static int
log2_lines(int lines)
{
  return 31 - __builtin_clz(lines);
}


static void
downsize_params(int src, int dst, uint32_t *ratio, uint32_t *div)
{
  uint32_t r = 8192 * src / dst;
  if(r < 8192)
    r = 8192;
  if(r > 65535)
    r = 65535;
  uint32_t d = (1024 * 8192 - 1) / r;
  if(d > 1023)
    d = 1023;
  if(d < 128)
    d = 128;
  *ratio = r;
  *div = d;
}


static void
set_yuv_matrix(void)
{
  // BT.601 limited range (what an H.264 decoder assumes without VUI).
  // Row R = Cr, row G = Y, row B = Cb, coefficients x256.
  reg_wr(DCMIPP_P1YUVRR1, DCMIPP_CC_R1(112, -94));
  reg_wr(DCMIPP_P1YUVRR2, DCMIPP_CC_R2(-18, 128));
  reg_wr(DCMIPP_P1YUVGR1, DCMIPP_CC_R1(66, 129));
  reg_wr(DCMIPP_P1YUVGR2, DCMIPP_CC_R2(25, 16));
  reg_wr(DCMIPP_P1YUVBR1, DCMIPP_CC_R1(-38, -74));
  reg_wr(DCMIPP_P1YUVBR2, DCMIPP_CC_R2(112, 128));
  reg_wr(DCMIPP_P1YUVCR, DCMIPP_CC_CLAMP | DCMIPP_CC_ENABLE);
}


error_t
stm32n6_dcmipp_pipe1_start(const stm32n6_dcmipp_pipe1_config_t *cfg,
                           void *buf0, void *buf1)
{
  const int ring = cfg->ring_lines;
  if((cfg->out_width & 15) || (cfg->out_height & 1) ||
     ((intptr_t)buf0 & 15) || (!ring && ((intptr_t)buf1 & 15)) ||
     (ring && ring != 16 && ring != 32 && ring != 64))
    return ERR_INVALID_ARGS;

  if(dcmipp.p1_running)
    stm32n6_dcmipp_pipe1_stop();

  const size_t uvoff = stm32n6_dcmipp_uv_offset(cfg);
  const size_t size = stm32n6_dcmipp_buffer_size(cfg);
  dcache_op(buf0, size, DCACHE_CLEAN | DCACHE_INVALIDATE);
  if(!ring)
    dcache_op(buf1, size, DCACHE_CLEAN | DCACHE_INVALIDATE);

  reg_wr(DCMIPP_P1FSCR,
         DCMIPP_FSCR_VC(cfg->vc) | DCMIPP_FSCR_DTMODE(DCMIPP_DTMODE_DTIDA) |
         DCMIPP_FSCR_DTIDA(cfg->dt));

  // ISP
  reg_wr(DCMIPP_P1SRCR, 0);
  reg_wr(DCMIPP_P1BPRCR, 0);
  reg_wr(DCMIPP_P1DECR, 0);
  const int bl = cfg->black_level;
  reg_wr(DCMIPP_P1BLCCR, DCMIPP_P1BLCCR_BLCR(bl) | DCMIPP_P1BLCCR_BLCG(bl) |
         DCMIPP_P1BLCCR_BLCB(bl) | DCMIPP_P1BLCCR_ENABLE);
  stm32n6_dcmipp_set_gains(256, 256, 256);
  reg_wr(DCMIPP_P1DMCR,
         DCMIPP_P1DMCR_EDGE(6) | DCMIPP_P1DMCR_LINEH(4) |
         DCMIPP_P1DMCR_LINEV(4) | DCMIPP_P1DMCR_PEAK(2) |
         DCMIPP_P1DMCR_TYPE(cfg->bayer) | DCMIPP_P1DMCR_ENABLE);
  stm32n6_dcmipp_set_ccm(NULL);

  // Statistics: average raw R, G and B over the centre of the crop
  dcmipp.stat_width = cfg->crop_width / 2;
  dcmipp.stat_height = cfg->crop_height / 2;
  reg_wr(DCMIPP_P1STSTR,
         DCMIPP_P1STSTR_VSTART(cfg->crop_y + cfg->crop_height / 4) |
         DCMIPP_P1STSTR_HSTART(cfg->crop_x + cfg->crop_width / 4));
  reg_wr(DCMIPP_P1STSZR, DCMIPP_P1STSZR_CROPEN |
         DCMIPP_P1STSZR_VSIZE(dcmipp.stat_height) |
         DCMIPP_P1STSZR_HSIZE(dcmipp.stat_width));
  reg_wr(DCMIPP_P1ST1CR, DCMIPP_P1STCR_SRC(DCMIPP_STSRC_R_PRE) |
         DCMIPP_P1STCR_ENABLE);
  reg_wr(DCMIPP_P1ST2CR, DCMIPP_P1STCR_SRC(DCMIPP_STSRC_G_PRE) |
         DCMIPP_P1STCR_ENABLE);
  reg_wr(DCMIPP_P1ST3CR, DCMIPP_P1STCR_SRC(DCMIPP_STSRC_B_PRE) |
         DCMIPP_P1STCR_ENABLE);

  // Post processing: crop, downsize, gamma, RGB to YUV
  reg_wr(DCMIPP_P1CRSTR, DCMIPP_PxCRSTR_VSTART(cfg->crop_y) |
         DCMIPP_PxCRSTR_HSTART(cfg->crop_x));
  reg_wr(DCMIPP_P1CRSZR, DCMIPP_PxCRSZR_ENABLE |
         DCMIPP_PxCRSZR_VSIZE(cfg->crop_height) |
         DCMIPP_PxCRSZR_HSIZE(cfg->crop_width));
  reg_wr(DCMIPP_P1DCCR, 0);

  uint32_t hratio, hdiv, vratio, vdiv;
  downsize_params(cfg->crop_width, cfg->out_width, &hratio, &hdiv);
  downsize_params(cfg->crop_height, cfg->out_height, &vratio, &vdiv);
  reg_wr(DCMIPP_P1DSRTIOR, DCMIPP_PxDSRTIOR_VRATIO(vratio) |
         DCMIPP_PxDSRTIOR_HRATIO(hratio));
  reg_wr(DCMIPP_P1DSSZR, DCMIPP_PxDSSZR_VSIZE(cfg->out_height) |
         DCMIPP_PxDSSZR_HSIZE(cfg->out_width));
  reg_wr(DCMIPP_P1DSCR, DCMIPP_PxDSCR_ENABLE |
         DCMIPP_PxDSCR_VDIV(vdiv) | DCMIPP_PxDSCR_HDIV(hdiv));

  reg_wr(DCMIPP_P1GMCR, DCMIPP_PxGMCR_ENABLE);
  set_yuv_matrix();

  if(ring) {
    // NV12 into a ring of two halves. The line event at the end of each
    // half is what the VENC is triggered by. The UV plane wraps after
    // the same number of luma lines, so its ring is half the size.
    reg_wr(DCMIPP_P1PPCR, DCMIPP_PPCR_FORMAT(DCMIPP_PPFMT_YUV420_2) |
           DCMIPP_PPCR_LINEMULT(log2_lines(ring)) |
           DCMIPP_PPCR_LMAWM(log2_lines(ring) + 1) | DCMIPP_PPCR_LMAWE);
  } else {
    // NV12, ping-pong between the two frames. LINEMULT at max, the line
    // event is hard wired to the VENC and would trigger it otherwise.
    reg_wr(DCMIPP_P1PPCR, DCMIPP_PPCR_FORMAT(DCMIPP_PPFMT_YUV420_2) |
           DCMIPP_PPCR_LINEMULT(7) | DCMIPP_PPCR_DBM);
  }
  reg_wr(DCMIPP_P1PPM0PR, cfg->out_width);
  reg_wr(DCMIPP_P1PPM1PR, cfg->out_width);
  reg_wr(DCMIPP_P1PPM0AR1, (intptr_t)buf0);
  reg_wr(DCMIPP_P1PPM1AR1, (intptr_t)buf0 + uvoff);
  if(!ring) {
    reg_wr(DCMIPP_P1PPM0AR2, (intptr_t)buf1);
    reg_wr(DCMIPP_P1PPM1AR2, (intptr_t)buf1 + uvoff);
  }
  dcmipp.p1_ring = ring;
  dcmipp.p1_line_events = 0;

  reg_wr(DCMIPP_CMFCR, DCMIPP_CM_P1FRAME | DCMIPP_CM_P1VSYNC |
         DCMIPP_CM_P1OVR | DCMIPP_CM_P1LINE);
  reg_or(DCMIPP_CMIER, DCMIPP_CM_P1FRAME | DCMIPP_CM_P1VSYNC |
         DCMIPP_CM_P1OVR | (ring ? DCMIPP_CM_P1LINE : 0));

  reg_wr(DCMIPP_P1FCTCR, 0); // Continuous, all frames
  reg_or(DCMIPP_P1FSCR, DCMIPP_FSCR_PIPEN);
  reg_or(DCMIPP_P1FCTCR, DCMIPP_FCTCR_CPTREQ);
  dcmipp.p1_running = 1;

  evlog(LOG_INFO, "dcmipp: %dx%d+%d+%d -> %dx%d NV12, %s",
        cfg->crop_width, cfg->crop_height, cfg->crop_x, cfg->crop_y,
        cfg->out_width, cfg->out_height,
        ring ? "line ring" : "frame buffers");
  return 0;
}


void
stm32n6_dcmipp_pipe1_stop(void)
{
  if(!dcmipp.p1_running)
    return;
  pipe_stop(DCMIPP_P1FCTCR, DCMIPP_P1FSCR, DCMIPP_CMSR1_P1CPTACT);
  reg_wr(DCMIPP_CMIER, reg_rd(DCMIPP_CMIER) &
         ~(DCMIPP_CM_P1FRAME | DCMIPP_CM_P1VSYNC | DCMIPP_CM_P1OVR |
           DCMIPP_CM_P1LINE));
  // Line events would keep triggering the VENC
  reg_wr(DCMIPP_P1PPCR, reg_rd(DCMIPP_P1PPCR) | DCMIPP_PPCR_LINEMULT(7));
  dcmipp.p1_running = 0;
}


void *
stm32n6_dcmipp_pipe1_wait(uint32_t *seq, int64_t deadline)
{
  void *r = NULL;
  const int q = irq_forbid(IRQ_LEVEL_IO);
  while(dcmipp.p1_seq == *seq) {
    if(task_sleep_deadline(&dcmipp.wait, deadline))
      goto out;
  }
  *seq = dcmipp.p1_seq;
  r = dcmipp.p1_last;
 out:
  irq_permit(q);
  return r;
}


uint32_t
stm32n6_dcmipp_pipe1_lines_events(void)
{
  return dcmipp.p1_line_events;
}


static uint32_t
gain_reg(int gain, int mult_shift, int shf_shift)
{
  // gain = MULT / 128 * 2^SHF
  uint32_t v = gain / 2;
  uint32_t shf = 0;
  while(v >= 256 && shf < 7) {
    v /= 2;
    shf++;
  }
  if(v > 255)
    v = 255;
  return (v << mult_shift) | (shf << shf_shift);
}


void
stm32n6_dcmipp_set_gains(int r, int g, int b)
{
  reg_wr(DCMIPP_P1EXCR1, gain_reg(r, 20, 28) | DCMIPP_P1EXCR1_ENABLE);
  reg_wr(DCMIPP_P1EXCR2, gain_reg(g, 20, 28) | gain_reg(b, 4, 12));
}


void
stm32n6_dcmipp_set_ccm(const int16_t m[3][3])
{
  if(m == NULL) {
    reg_wr(DCMIPP_P1CCCR, 0);
    return;
  }
  reg_wr(DCMIPP_P1CCRR1, DCMIPP_CC_R1(m[0][0], m[0][1]));
  reg_wr(DCMIPP_P1CCRR2, DCMIPP_CC_R2(m[0][2], 0));
  reg_wr(DCMIPP_P1CCGR1, DCMIPP_CC_R1(m[1][0], m[1][1]));
  reg_wr(DCMIPP_P1CCGR2, DCMIPP_CC_R2(m[1][2], 0));
  reg_wr(DCMIPP_P1CCBR1, DCMIPP_CC_R1(m[2][0], m[2][1]));
  reg_wr(DCMIPP_P1CCBR2, DCMIPP_CC_R2(m[2][2], 0));
  reg_wr(DCMIPP_P1CCCR, DCMIPP_CC_ENABLE);
}


void
stm32n6_dcmipp_get_stats(stm32n6_dcmipp_stats_t *st)
{
  const int q = irq_forbid(IRQ_LEVEL_IO);
  *st = dcmipp.stats;
  irq_permit(q);
}


void
stm32n6_dcmipp_print_status(stream_t *st)
{
  if(!dcmipp.initialized) {
    stprintf(st, "  Not initialized\n");
    return;
  }
  stprintf(st, "  Pipe1: %s%s  Frames:%u VSYNC:%u Overruns:%u AXI errors:%u\n",
           dcmipp.p1_running ? "running" : "stopped",
           dcmipp.p1_running && dcmipp.p1_ring ? " (line ring)" : "",
           dcmipp.p1_seq, dcmipp.vsync, dcmipp.overruns,
           dcmipp.axi_errors);
  const uint32_t ex1 = reg_rd(DCMIPP_P1EXCR1);
  const uint32_t ex2 = reg_rd(DCMIPP_P1EXCR2);
  stprintf(st, "  Raw average R:%d G:%d B:%d  "
           "Gains R:%u<<%u G:%u<<%u B:%u<<%u (/128)  CCM:%s\n",
           dcmipp.stats.r, dcmipp.stats.g, dcmipp.stats.b,
           (ex1 >> 20) & 0xff, ex1 >> 28,
           (ex2 >> 20) & 0xff, ex2 >> 28,
           (ex2 >> 4) & 0xff, (ex2 >> 12) & 7,
           reg_rd(DCMIPP_P1CCCR) & DCMIPP_CC_ENABLE ? "on" : "off");
  stprintf(st, "  CMSR1:0x%08x CMSR2:0x%08x P1SR:0x%08x\n",
           reg_rd(DCMIPP_CMSR1), reg_rd(DCMIPP_CMSR2), reg_rd(DCMIPP_P1SR));
}
