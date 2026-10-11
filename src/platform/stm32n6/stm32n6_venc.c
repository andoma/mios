#include "stm32n6_venc.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <malloc.h>
#include <sys/param.h>

#include <mios/mios.h>
#include <mios/task.h>
#include <mios/eventlog.h>
#include <mios/type_macros.h>
#include <unistd.h>

#include "cache.h"
#include "irq.h"
#include "stm32n6_clk.h"
#include "stm32n6_rif.h"
#include "stm32n6_syscfg.h"
#include "stm32n6_venc_reg.h"

#define VENC_IRQ 62

#define RIF_MASTER_VENC     12
#define RIF_PERIPH_VENC_REG 3
#define RIF_PERIPH_VENC_BIT 1

#define SYSCFG_VENCRAMCR (SYSCFG_BASE + 0x20)

#define ALIGN(x, a) (((x) + (a) - 1) & ~((a) - 1))

// SWREG0 .. SWREG497
#define VENC_NUM_REGS 498

// Mode decision tables indexed by QP, from the VeriSilicon encoder
// package in STM32CubeN6 (H264CodeFrame.c, BSD-3-Clause)
static const uint16_t h264Intra16Favor[52] = {
  24, 24, 24, 26, 27, 30, 32, 35, 39, 43, 48, 53, 58, 64, 71, 78,
  85, 93, 102, 111, 121, 131, 142, 154, 167, 180, 195, 211, 229, 248, 271,
  296, 326, 361, 404, 457, 523, 607, 714, 852, 1034, 1272, 1588, 2008,
  2568, 3318, 4323, 5672, 7486, 9928, 13216, 17648
};

static const uint8_t h264PrevModeFavor[52] = {
  7, 7, 8, 8, 9, 9, 10, 10, 11, 12, 12, 13, 14, 15, 16, 17, 18, 19,
  20, 21, 22, 24, 25, 27, 29, 30, 32, 34, 36, 38, 41, 43, 46, 49, 51, 55,
  58, 61, 65, 69, 73, 78, 82, 87, 93, 98, 104, 110, 117, 124, 132, 140
};

static const uint16_t h264InterFavor[52] = {
  4, 4, 5, 6, 6, 7, 8, 9, 10, 12, 13, 15, 17, 19, 21, 24, 26, 30, 34, 38,
  42, 48, 53, 60, 68, 76, 85, 96, 107, 121, 136, 152, 171, 192, 215, 242,
  272, 305, 342, 384, 431, 484, 544, 610, 685, 769, 863, 969, 1088, 1221,
  1370, 1538
};

static const uint8_t h264SkipSadPenalty[52] = {
  255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 240,
  224, 208, 192, 176, 160, 144, 128, 112, 96, 80, 64, 60, 56, 52, 48, 44,
  40, 36, 32, 28, 24, 20, 18, 16, 16, 15, 15, 14, 14, 13, 13, 12, 12, 11,
  11, 10, 10, 9, 9, 8
};

// sqrt(2^((qp-12)/3))*8
static const uint16_t h264DiffMvPenalty[52] = {
  2, 2, 3, 3, 3, 4, 4, 4, 5, 6, 6, 7, 8, 9, 10, 11, 13, 14, 16, 18, 20,
  23, 26, 29, 32, 36, 40, 45, 51, 57, 64, 72, 81, 91, 102, 114, 128, 144,
  161, 181, 203, 228, 256, 287, 323, 362, 406, 456, 512, 575, 645, 724
};

// 31*sqrt(2^((qp-12)/3))/4
static const uint16_t h264DiffMvPenalty4p[52] = {
  2, 2, 2, 3, 3, 3, 4, 4, 5, 5, 6, 7, 8, 9, 10, 11, 12, 14, 16, 17, 20,
  22, 25, 28, 31, 35, 39, 44, 49, 55, 62, 70, 78, 88, 98, 110, 124, 139,
  156, 175, 197, 221, 248, 278, 312, 351, 394, 442, 496, 557, 625, 701
};

// 4*sqrt(i*4*6)
static const uint8_t h264DmvPenalty[128] = {
  0, 19, 27, 33, 39, 43, 48, 51, 55, 58, 61, 64, 67, 70, 73, 75, 78, 80,
  83, 85, 87, 89, 91, 93, 96, 97, 99, 101, 103, 105, 107, 109, 110, 112,
  114, 115, 117, 119, 120, 122, 123, 125, 126, 128, 129, 131, 132, 134,
  135, 137, 138, 139, 141, 142, 144, 145, 146, 147, 149, 150, 151, 153,
  154, 155, 156, 157, 159, 160, 161, 162, 163, 165, 166, 167, 168, 169,
  170, 171, 173, 174, 175, 176, 177, 178, 179, 180, 181, 182, 183, 184,
  185, 186, 187, 188, 189, 190, 192, 192, 193, 194, 195, 196, 197, 198,
  199, 200, 201, 202, 203, 204, 205, 206, 207, 208, 209, 210, 211, 211,
  212, 213, 214, 215, 216, 217, 218, 219, 219, 220
};


struct stm32n6_venc_h264 {
  uint16_t width;
  uint16_t height;
  uint16_t stride;
  uint8_t mb_width;
  uint8_t mb_height;
  uint8_t qp;
  uint8_t pps_qp;     // pic_init_qp in the last PPS sent
  uint8_t level;
  uint16_t gop;
  uint16_t intra_refresh;  // Frames per sweep, 0 = off
  uint8_t slice_rows;      // 0 = one slice per frame
  uint16_t ir_pos;         // Frame within the current sweep
  uint8_t stream_mb_rows;

  uint16_t frame_num;
  uint8_t idr_pic_id;
  uint16_t frames_since_idr;
  uint8_t need_idr;

  uint8_t *luma[2];   // [0] = reference, [1] = reconstruction
  uint8_t *chroma[2];

  uint8_t *out;
  size_t out_size;

  // Auxiliary buffers. Not needed for CAVLC without MV output, but ST's
  // library always provides them, so do the same
  uint8_t *aux;
  size_t aux_size;
  uint32_t nal_tbl_offset;
  uint32_t cabac_offset;
  uint32_t mv_offset;
  uint32_t segmap_offset;

  // Register image, written in full for every frame
  uint32_t regs[VENC_NUM_REGS];

  uint32_t frames;
  uint32_t errors;
  uint32_t last_size;
  uint32_t encode_time;
};


static struct {
  mutex_t mutex;
  task_waitable_t wait;
  uint32_t status;
  uint8_t initialized;
} venc;


static void
venc_irq(void *arg)
{
  const uint32_t status = reg_rd(VENC_SWREG(1));
  reg_wr(VENC_SWREG(1), status); // Write one to clear
  venc.status |= status;
  task_wakeup(&venc.wait, 0);
}


static error_t
venc_hw_init(void)
{
  if(venc.initialized)
    return 0;

  // VENCRAM belongs to the VENC (reset default)
  reg_clr_bit(SYSCFG_VENCRAMCR, 0);

  // Erratum ES0620 2.2.6: VENCEN alone does not start the VENC clock,
  // APB5EN in RCC_BUSENR must be set too
  reg_set_bit(RCC_BUSENR, 12);
  reg_set_bit(RCC_MEMENR, 11); // VENCRAMEN
  clk_enable(CLK_VENC);
  reg_set_bit(RCC_APB5LPENR, 5);
  rst_assert(CLK_VENC);
  rst_deassert(CLK_VENC);

  // Secure peripheral and secure AXI master, same as ETH1. While the
  // peripheral is nonsecure its AXI accesses are forced nonsecure, and
  // those are rejected by the RISAFs in front of the AXISRAMs.
  stm32n6_rif_set_periph_sec(RIF_PERIPH_VENC_REG, RIF_PERIPH_VENC_BIT, 1, 1);
  stm32n6_rif_set_master_attr(RIF_MASTER_VENC, 1, 1, 1);

  const uint32_t id = reg_rd(VENC_SWREG(0));
  if(id != VENC_ID_V5) {
    evlog(LOG_ERR, "venc: Unexpected ID 0x%08x", id);
    clk_disable(CLK_VENC);
    return ERR_NO_DEVICE;
  }

  mutex_init(&venc.mutex, "venc");
  task_waitable_init(&venc.wait, "venc");
  irq_enable_fn_arg(VENC_IRQ, IRQ_LEVEL_IO, venc_irq, NULL);
  venc.initialized = 1;
  return 0;
}


// Bit writer for the parameter sets, with emulation prevention
typedef struct {
  uint8_t *buf;
  size_t pos;
  uint32_t acc;
  int bits;
  int zeros;
} bitwriter_t;

static void
bw_byte(bitwriter_t *bw, uint8_t b)
{
  if(bw->zeros >= 2 && b <= 3) {
    bw->buf[bw->pos++] = 3;
    bw->zeros = 0;
  }
  bw->buf[bw->pos++] = b;
  bw->zeros = b ? 0 : bw->zeros + 1;
}

static void
bw_put(bitwriter_t *bw, uint32_t value, int bits)
{
  while(bits--) {
    bw->acc = (bw->acc << 1) | ((value >> bits) & 1);
    if(++bw->bits == 8) {
      bw_byte(bw, bw->acc);
      bw->acc = 0;
      bw->bits = 0;
    }
  }
}

static void
bw_ue(bitwriter_t *bw, uint32_t v)
{
  v++;
  int len = 32 - __builtin_clz(v);
  bw_put(bw, 0, len - 1);
  bw_put(bw, v, len);
}

static void
bw_se(bitwriter_t *bw, int v)
{
  bw_ue(bw, v > 0 ? 2 * v - 1 : -2 * v);
}

static void
bw_trailing(bitwriter_t *bw)
{
  bw_put(bw, 1, 1);
  if(bw->bits)
    bw_put(bw, 0, 8 - bw->bits);
}

static void
bw_nal_start(bitwriter_t *bw, uint8_t nal_header)
{
  // Start code and NAL header are not subject to emulation prevention
  bw->buf[bw->pos++] = 0;
  bw->buf[bw->pos++] = 0;
  bw->buf[bw->pos++] = 0;
  bw->buf[bw->pos++] = 1;
  bw->buf[bw->pos++] = nal_header;
  bw->zeros = 0;
}


// Write SPS + PPS. Several values are implied by the hardware's slice
// header generation (16 bit frame_num, POC type 2, one reference frame)
// and must match what ST's encoder library writes.
static int
ue_len(uint32_t v)
{
  return 2 * (31 - __builtin_clz(v + 1)) + 1;
}


static size_t
write_parameter_sets(stm32n6_venc_h264_t *v, uint8_t *buf,
                     int recovery_point)
{
  bitwriter_t bw = { .buf = buf };

  bw_nal_start(&bw, 0x67);           // nal_ref_idc 3, SPS
  bw_put(&bw, 66, 8);                // profile_idc: Baseline
  bw_put(&bw, 0xe0, 8);              // constraint_set0/1/2
  bw_put(&bw, v->level, 8);
  bw_ue(&bw, 0);                     // seq_parameter_set_id
  bw_ue(&bw, 12);                    // log2_max_frame_num_minus4
  bw_ue(&bw, 2);                     // pic_order_cnt_type
  bw_ue(&bw, 1);                     // max_num_ref_frames
  bw_put(&bw, 0, 1);                 // gaps_in_frame_num_value_allowed
  bw_ue(&bw, v->mb_width - 1);
  bw_ue(&bw, v->mb_height - 1);
  bw_put(&bw, 1, 1);                 // frame_mbs_only_flag
  bw_put(&bw, 1, 1);                 // direct_8x8_inference_flag
  const int crop_right = (v->mb_width * 16 - v->width) / 2;
  const int crop_bottom = (v->mb_height * 16 - v->height) / 2;
  if(crop_right || crop_bottom) {
    bw_put(&bw, 1, 1);
    bw_ue(&bw, 0);
    bw_ue(&bw, crop_right);
    bw_ue(&bw, 0);
    bw_ue(&bw, crop_bottom);
  } else {
    bw_put(&bw, 0, 1);
  }
  bw_put(&bw, 0, 1);                 // vui_parameters_present_flag
  bw_trailing(&bw);

  bw_nal_start(&bw, 0x68);           // nal_ref_idc 3, PPS
  bw_ue(&bw, 0);                     // pic_parameter_set_id
  bw_ue(&bw, 0);                     // seq_parameter_set_id
  bw_put(&bw, 0, 1);                 // entropy_coding_mode_flag: CAVLC
  bw_put(&bw, 0, 1);                 // bottom_field_pic_order_in_frame_present
  bw_ue(&bw, 0);                     // num_slice_groups_minus1
  bw_ue(&bw, 0);                     // num_ref_idx_l0_default_active_minus1
  bw_ue(&bw, 0);                     // num_ref_idx_l1_default_active_minus1
  bw_put(&bw, 0, 1);                 // weighted_pred_flag
  bw_put(&bw, 0, 2);                 // weighted_bipred_idc
  v->pps_qp = v->qp;
  bw_se(&bw, v->pps_qp - 26);        // pic_init_qp_minus26
  bw_se(&bw, 0);                     // pic_init_qs_minus26
  bw_se(&bw, 0);                     // chroma_qp_index_offset
  bw_put(&bw, 1, 1);                 // deblocking_filter_control_present
  bw_put(&bw, 0, 1);                 // constrained_intra_pred_flag
  bw_put(&bw, 0, 1);                 // redundant_pic_cnt_present_flag
  bw_trailing(&bw);

  if(recovery_point) {
    // Recovery point SEI: the picture is complete again after one sweep.
    // Not an exact match, motion vectors can reach into the area that
    // hasn't been refreshed yet.
    const uint32_t cnt = v->intra_refresh - 1;
    const int bits = ue_len(cnt) + 4;
    bw_nal_start(&bw, 0x06);
    bw_put(&bw, 6, 8);               // payloadType: recovery_point
    bw_put(&bw, (bits + 7) / 8, 8);  // payloadSize
    bw_ue(&bw, cnt);                 // recovery_frame_cnt
    bw_put(&bw, 0, 1);               // exact_match_flag
    bw_put(&bw, 0, 1);               // broken_link_flag
    bw_put(&bw, 0, 2);               // changing_slice_group_idc
    if(bw.bits) {
      bw_put(&bw, 1, 1);             // bit_equal_to_one
      if(bw.bits)
        bw_put(&bw, 0, 8 - bw.bits); // bit_equal_to_zero
    }
    bw_trailing(&bw);
  }

  // The hardware writes 64 bit words, so pad with zero bytes (legal
  // between NAL units in a byte stream) to keep its part aligned
  while(bw.pos & 7)
    buf[bw.pos++] = 0;
  return bw.pos;
}


static int
level_for(int mbs_per_sec, int mbs)
{
  static const struct {
    uint32_t mbps;
    uint16_t frame_mbs;
    uint8_t level;
  } levels[] = {
    {   11880,  396, 20 },
    {   19800,  792, 21 },
    {   20250, 1620, 22 },
    {   40500, 1620, 30 },
    {  108000, 3600, 31 },
    {  216000, 5120, 32 },
    {  245760, 8192, 40 },
    {  522240, 8704, 42 },
  };
  for(size_t i = 0; i < ARRAYSIZE(levels); i++) {
    if(mbs_per_sec <= levels[i].mbps && mbs <= levels[i].frame_mbs)
      return levels[i].level;
  }
  return 51;
}


static int
exp_golomb_signed_len(int val)
{
  val = val > 0 ? 2 * val : -2 * val + 1;
  int len = 0;
  while(val >> ++len);
  return len * 2 - 1;
}


stm32n6_venc_h264_t *
stm32n6_venc_h264_create(const stm32n6_venc_h264_config_t *cfg)
{
  if(cfg->width < 144 || cfg->width > 1920 || (cfg->width & 3) ||
     cfg->height < 96 || cfg->height > 1088 || (cfg->height & 1) ||
     cfg->qp > 51) {
    return NULL;
  }

  const int mb_width = (cfg->width + 15) / 16;
  if(cfg->stream_mb_rows && (mb_width * cfg->stream_mb_rows) & 3)
    return NULL; // Hardware requirement

  if(venc_hw_init())
    return NULL;

  stm32n6_venc_h264_t *v = calloc(1, sizeof(stm32n6_venc_h264_t));
  v->width = cfg->width;
  v->height = cfg->height;
  v->stride = ALIGN(cfg->width, 16);
  v->mb_width = (cfg->width + 15) / 16;
  v->mb_height = (cfg->height + 15) / 16;
  v->qp = cfg->qp;
  v->gop = cfg->gop ?: 30;
  v->intra_refresh = MIN(cfg->intra_refresh, v->mb_height);
  stm32n6_venc_h264_set_slice_rows(v, cfg->slice_rows);
  v->stream_mb_rows = cfg->stream_mb_rows;
  v->need_idr = 1;

  const int mbs = v->mb_width * v->mb_height;
  v->level = level_for(mbs * (cfg->fps ?: 30), mbs);

  // Reference and reconstruction, swapped after every frame
  for(int i = 0; i < 2; i++) {
    v->luma[i] = xalloc(mbs * 256, 64, cfg->ref_mem_type);
    v->chroma[i] = xalloc(mbs * 128, 64, cfg->ref_mem_type);
  }

  // Worst case for a sane QP is well below this. A frame that doesn't fit
  // is reported as an error and the next one is forced to IDR.
  v->out_size = ALIGN(v->width * v->height / 2 + 4096, 64);
  v->out = xalloc(v->out_size, 64, MEM_TYPE_DMA);

  // NAL size table, CABAC context tables, MV/MB info output (56 bytes per
  // MB on this revision) and the segmentation map (4 bits per MB)
  v->nal_tbl_offset = 0;
  v->cabac_offset = ALIGN((v->mb_height + 4) * 4, 64);
  v->mv_offset = v->cabac_offset + ALIGN(52 * 2 * 464, 64);
  v->segmap_offset = v->mv_offset + ALIGN(mbs * 64, 64);
  v->aux_size = v->segmap_offset + ALIGN((mbs * 4 + 63) / 64 * 8, 64);
  v->aux = xalloc(v->aux_size, 64, cfg->ref_mem_type);

  if(v->luma[0] == NULL || v->luma[1] == NULL ||
     v->chroma[0] == NULL || v->chroma[1] == NULL || v->out == NULL ||
     v->aux == NULL) {
    evlog(LOG_ERR, "venc: Out of memory");
    stm32n6_venc_h264_destroy(v);
    return NULL;
  }

  // The CPU never touches these, but make sure no dirty lines get
  // evicted on top of what the encoder writes
  for(int i = 0; i < 2; i++) {
    dcache_op(v->luma[i], mbs * 256, DCACHE_CLEAN | DCACHE_INVALIDATE);
    dcache_op(v->chroma[i], mbs * 128, DCACHE_CLEAN | DCACHE_INVALIDATE);
  }
  memset(v->aux, 0, v->aux_size);
  dcache_op(v->aux, v->aux_size, DCACHE_CLEAN | DCACHE_INVALIDATE);

  evlog(LOG_INFO, "venc: H.264 %dx%d level %d.%d qp %d gop %d ref@%p",
        v->width, v->height, v->level / 10, v->level % 10, v->qp, v->gop,
        v->luma[0]);
  return v;
}


void
stm32n6_venc_h264_set_slice_rows(stm32n6_venc_h264_t *v, int rows)
{
  // The field is 7 bits, and a slice can't be taller than the picture
  if(rows < 0 || rows >= v->mb_height)
    rows = 0;
  if(rows > 127)
    rows = 127;
  v->slice_rows = rows;
}


void
stm32n6_venc_h264_destroy(stm32n6_venc_h264_t *v)
{
  for(int i = 0; i < 2; i++) {
    free(v->luma[i]);
    free(v->chroma[i]);
  }
  free(v->out);
  free(v->aux);
  free(v);
}


void
stm32n6_venc_h264_set_qp(stm32n6_venc_h264_t *v, int qp)
{
  if(qp < 0)
    qp = 0;
  if(qp > 51)
    qp = 51;
  v->qp = qp;
}


static void
venc_program_tables(uint32_t *regs)
{
  for(int i = 0; i < 128; i += 4) {
    uint32_t dmv = 0, qpel = 0;
    for(int j = 0; j < 4; j++) {
      const int p = (h264DmvPenalty[i + j] + 1) / 2;
      int q = exp_golomb_signed_len(i + j);
      if(q > 255)
        q = 255;
      dmv |= p << (24 - j * 8);
      qpel |= q << (24 - j * 8);
    }
    regs[96 + i / 4] = dmv;
    regs[128 + i / 4] = qpel;
  }
}


static void
venc_dump_regs(stream_t *st)
{
  if(st == NULL)
    return;
  for(int i = 0; i < VENC_NUM_REGS; i++) {
    const uint32_t r = reg_rd(VENC_SWREG(i));
    if(r)
      stprintf(st, "SWREG%-3d (0x%03x) = 0x%08x\n", i, i * 4, r);
  }
}


error_t
stm32n6_venc_h264_encode(stm32n6_venc_h264_t *v, const void *y, const void *uv,
                         int force_idr, const uint8_t **outp, size_t *outlen)
{
  if(((intptr_t)y & 7) || ((intptr_t)uv & 7))
    return ERR_INVALID_ARGS;

  const int idr = force_idr || v->need_idr ||
    (!v->intra_refresh && v->frames_since_idr >= v->gop);
  const int qp = v->qp;
  const uint32_t mbw = v->mb_width;
  const uint32_t mbh = v->mb_height;

  // Intra refresh band for this frame: rows [ir_top, ir_bottom], one row
  // of overlap with the next band
  const int ir = v->intra_refresh && !idr;
  if(idr)
    v->ir_pos = 0;
  uint32_t ir_top = 0, ir_bottom = 0;
  if(ir) {
    ir_top = v->ir_pos * mbh / v->intra_refresh;
    ir_bottom = MIN((v->ir_pos + 1) * mbh / v->intra_refresh, mbh - 1);
  }

  size_t hdrlen = 0;
  if(idr) {
    v->frame_num = 0;
    hdrlen = write_parameter_sets(v, v->out, 0);
  } else if(ir && v->ir_pos == 0) {
    // A receiver can start decoding from here
    hdrlen = write_parameter_sets(v, v->out, 1);
  }

  // Push the headers out and make sure no dirty lines are left on top of
  // what the encoder is about to write
  dcache_op(v->out, v->out_size, DCACHE_CLEAN | DCACHE_INVALIDATE);

  mutex_lock(&venc.mutex);
  const int64_t t0 = clock_get();

  // In streaming mode the encoder legitimately waits for the camera, so
  // the hardware timeout is off (as in ST's library)
  const uint32_t ctrl =
    (v->stream_mb_rows ? 0 : VENC_CTRL_INT_TIMEOUT) |
    VENC_CTRL_MV_WRITE |
    VENC_CTRL_NAL_SIZE_WRITE |
    VENC_CTRL_WIDTH(mbw) |
    VENC_CTRL_HEIGHT(mbh) |
    VENC_CTRL_REC_WRITE_BUF |
    (idr ? VENC_CTRL_PIC_INTRA : VENC_CTRL_PIC_INTER) |
    VENC_CTRL_MODE_H264;

  uint32_t *R = v->regs;
  memset(R, 0, sizeof(v->regs));
  const uint32_t aux = (intptr_t)v->aux;

  R[1] = VENC_STATUS_ALL | VENC_IRQ_LINE;
  R[2] = VENC_SWREG2_DEFAULT;
  R[3] = VENC_SWREG3_DEFAULT;
  R[5] = (intptr_t)v->out + hdrlen;
  R[6] = aux + v->nal_tbl_offset;
  R[7] = (intptr_t)v->luma[0];
  R[8] = (intptr_t)v->chroma[0];
  R[9] = (intptr_t)v->luma[1];
  R[10] = (intptr_t)v->chroma[1];
  R[11] = (intptr_t)y;
  R[12] = (intptr_t)uv;
  R[13] = (intptr_t)uv; // Unused for NV12

  const int xfill = (v->width & 15) ? (16 - (v->width & 15)) / 4 : 0;
  const int yfill = (v->height & 15) ? 16 - (v->height & 15) : 0;
  R[15] = VENC_PP_ROW_LENGTH(v->stride) |
    VENC_PP_XFILL(xfill) |
    VENC_PP_YFILL(yfill) |
    VENC_PP_FMT_NV12;

  R[16] = VENC_H264_PIC_INIT_QP(v->pps_qp) |
    VENC_H264_IDR_PIC_ID(v->idr_pic_id & 0xf);

  R[17] = VENC_H264_PPS_ID(0) |
    VENC_H264_PREV_MODE_FAVOR(h264PrevModeFavor[qp]) |
    VENC_H264_FRAME_NUM(v->frame_num);

  R[18] = VENC_H264_DEBLOCKING(0) |
    VENC_H264_SLICE_SIZE(v->slice_rows) |
    (v->level >= 31 ? VENC_H264_INTER4_RESTRICT : 0) |
    VENC_H264_INTRA16_FAVOR(h264Intra16Favor[qp]);

  int dmv4p = (h264DiffMvPenalty4p[qp] / 10 + 4) / 8;
  int dmv1p = ((4 * h264DiffMvPenalty[qp]) / 5 + 4) / 8;
  R[19] = VENC_SPLIT_MV |
    VENC_DMV_PENALTY_QP(h264DiffMvPenalty[qp]) |
    VENC_DMV_PENALTY_4P(dmv4p ?: 1) |
    VENC_DMV_PENALTY_1P(dmv1p ?: 1);

  R[20] = 0;
  R[21] = VENC_SKIP_PENALTY(h264SkipSadPenalty[qp]) |
    VENC_INTER_FAVOR(h264InterFavor[qp]);

  // Header remainder not used, the software part is padded to 8 bytes
  R[22] = 0;
  R[23] = 0;

  // Limit in 8 byte units, with the same 64 byte margin as ST
  const uint32_t limit = ((v->out_size - hdrlen) / 8) & ~7;
  R[24] = limit - 8;

  R[25] = 0; // MAD QP adjustment off
  // Intra macroblocks get a slightly lower QP (the band is also ROI 1),
  // as in ST's library, so the band doesn't show. MIN_QP has to allow it.
  const int roi_dqp = ir ? MIN(3, qp) : 0;
  R[27] = VENC_QP(qp) | VENC_QP_MAX(qp) | VENC_QP_MIN(qp - roi_dqp);
  R[36] = VENC_LONG_TERM_PENALTY(MIN(255, h264DiffMvPenalty[qp] / 2));
  R[37] = VENC_START_OFFSET(0);
  // ST's register descriptions say right/bottom are outside the area
  // while its API treats them as inside. Set them one past the band,
  // which is right either way (at worst one more row of overlap).
  if(ir)
    R[56] = VENC_AREA(0, mbw, ir_top, ir_bottom + 1);
  else
    R[56] = VENC_AREA(mbw, mbw, mbh, mbh);
  R[57] = 0;
  R[58] = 0;
  R[59] = 0;
  if(ir)
    R[60] = VENC_AREA(0, mbw, ir_top, ir_bottom + 1);
  else
    R[60] = VENC_AREA(mbw, mbw, mbh, mbh);
  R[61] = VENC_AREA(mbw, mbw, mbh, mbh);
  R[62] = VENC_ZERO_MV_FAVOR(10) | VENC_ROI1_DELTA_QP(roi_dqp);

  venc_program_tables(R);

  R[294] = VENC_SWREG294_DEFAULT;
  R[295] = VENC_SWREG295_DEFAULT;
  R[297] = 10 << 23; // MB rate control QP delta range
  R[410] = 0;        // MB rate control off
  R[422] = VENC_SWREG422_DEFAULT;
  if(v->stream_mb_rows) {
    R[497] = VENC_LL_ENABLE | VENC_LL_HW_HANDSHAKE | VENC_LL_LOOPBACK |
      VENC_LL_SYNC_MB_ROWS(v->stream_mb_rows);
  }

  R[51] = aux + v->cabac_offset;
  R[52] = aux + v->mv_offset;
  R[71] = aux + v->segmap_offset;

  // Segments 1-3 get the same penalties as segment 0 (segments are not
  // used, this is what ST's library ends up with)
  for(int seg = 0; seg < 3; seg++) {
    uint32_t *S = R + 256 + seg * 12;
    S[4] = h264PrevModeFavor[qp] << 20;
    S[5] = h264Intra16Favor[qp] << 12;
    S[6] = (MIN(255, h264DiffMvPenalty[qp] / 2) << 24) |
      (h264SkipSadPenalty[qp] << 16) | h264InterFavor[qp];
    S[8] = h264DiffMvPenalty[qp] << 20;
  }

  R[14] = ctrl;

  // Write everything but the read only config registers, in the same
  // order as ST's library. The control register goes first since some
  // registers alias between codecs.
  reg_wr(VENC_SWREG(14), ctrl);
  for(int i = 1; i < VENC_NUM_REGS; i++) {
    if(i == 63 || i == 296 || (i >= 164 && i <= 230))
      continue;
    reg_wr(VENC_SWREG(i), R[i]);
  }


  const int q = irq_forbid(IRQ_LEVEL_IO);
  venc.status = 0;
  reg_wr(VENC_SWREG(14), ctrl | VENC_CTRL_ENABLE);

  const int64_t deadline = t0 + 500000;
  error_t err = 0;
  while(!(venc.status & (VENC_STATUS_FRAME_RDY | VENC_STATUS_BUS_ERR |
                         VENC_STATUS_BUF_FULL | VENC_STATUS_TIMEOUT |
                         VENC_STATUS_RESET))) {
    if(task_sleep_deadline(&venc.wait, deadline)) {
      err = ERR_TIMEOUT;
      break;
    }
  }
  const uint32_t status = venc.status;
  irq_permit(q);

  if(err == 0 && !(status & VENC_STATUS_FRAME_RDY))
    err = status & VENC_STATUS_BUF_FULL ? ERR_NO_BUFFER : ERR_OPERATION_FAILED;

  size_t len = 0;
  if(!err) {
    len = hdrlen + reg_rd(VENC_SWREG(24)) / 8;
    if(len > v->out_size)
      err = ERR_OPERATION_FAILED;
  }

  if(err) {
    // Dump the registers on the first failure, for debugging
    if(v->errors == 0) {
      evlog(LOG_WARNING, "venc: Status 0x%x, registers:", status);
      venc_dump_regs(stdio);
    }
    // Recover with a block reset, settings are rewritten every frame
    reg_wr(VENC_SWREG(14), 0);
    rst_assert(CLK_VENC);
    rst_deassert(CLK_VENC);
  }

  v->encode_time = clock_get() - t0;
  mutex_unlock(&venc.mutex);

  if(err) {
    v->errors++;
    v->need_idr = 1;
    evlog(LOG_WARNING, "venc: Frame failed, status 0x%x err %d", status, err);
    return err;
  }

  dcache_op(v->out, len, DCACHE_INVALIDATE);

  // Reconstruction becomes the reference for the next frame
  uint8_t *tmp;
  tmp = v->luma[0]; v->luma[0] = v->luma[1]; v->luma[1] = tmp;
  tmp = v->chroma[0]; v->chroma[0] = v->chroma[1]; v->chroma[1] = tmp;

  if(idr) {
    v->idr_pic_id++;
    v->frames_since_idr = 0;
    v->need_idr = 0;
  }
  v->frames_since_idr++;
  if(ir)
    v->ir_pos = (v->ir_pos + 1) % v->intra_refresh;
  v->frame_num++;
  v->frames++;
  v->last_size = len;

  *outp = v->out;
  *outlen = len;
  return 0;
}


void
stm32n6_venc_h264_print_status(stm32n6_venc_h264_t *v, stream_t *st)
{
  stprintf(st, "  H.264 Baseline %dx%d level %d.%d  QP:%d  Input: %s\n",
           v->width, v->height, v->level / 10, v->level % 10, v->qp,
           v->stream_mb_rows ? "DCMIPP stream" : "frame");
  if(v->slice_rows)
    stprintf(st, "  Slices: %d macroblock rows each, %d per frame\n",
             v->slice_rows,
             (v->mb_height + v->slice_rows - 1) / v->slice_rows);
  else
    stprintf(st, "  Slices: one per frame\n");
  if(v->intra_refresh)
    stprintf(st, "  Intra refresh: sweep over %d frames (now at %d)\n",
             v->intra_refresh, v->ir_pos);
  else
    stprintf(st, "  IDR every %d frames\n", v->gop);
  stprintf(st, "  Frames:%u Errors:%u  Last frame: %u bytes, %u us\n",
           v->frames, v->errors, v->last_size, v->encode_time);
}


void
stm32n6_venc_dump_regs(stream_t *st)
{
  venc_dump_regs(st);
}
