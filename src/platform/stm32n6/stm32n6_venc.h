#pragma once

#include <stddef.h>
#include <stdint.h>

#include <mios/error.h>

struct stream;

// H.264 Baseline (CAVLC) encoder on the STM32N6 VENC. Input is NV12 with
// a luma stride of ALIGN16(width).

typedef struct stm32n6_venc_h264 stm32n6_venc_h264_t;

typedef struct {
  uint16_t width;
  uint16_t height;
  uint8_t fps;
  uint8_t qp;        // Fixed QP (0 - 51)
  uint16_t gop;      // IDR interval in frames (unless intra refresh)
  // Gradual intra refresh: a band of intra macroblock rows sweeps down
  // the picture, refreshing all of it in this many frames. The stream
  // then only has an IDR at the start (and after errors). SPS, PPS and a
  // recovery point SEI are sent at the start of every sweep. 0 = off.
  uint16_t intra_refresh;
  uint8_t slice_rows;  // Macroblock rows per slice, 0 = one slice
  // Memory type for the reference/reconstruction frames (only touched
  // by the VENC, so they don't need to be in the DMA heap as long as the
  // VENC AXI master can reach them)
  uint32_t ref_mem_type;
  // 0: Encode from a whole frame in memory.
  // N: Stream from the DCMIPP via a ring of two halves of N macroblock
  //    rows each. Each half is encoded when the DCMIPP signals that it's
  //    been written (venc_rdy), so encode() must be called before the
  //    first lines of the frame arrive.
  uint8_t stream_mb_rows;
} stm32n6_venc_h264_config_t;

stm32n6_venc_h264_t *stm32n6_venc_h264_create(const stm32n6_venc_h264_config_t *cfg);

void stm32n6_venc_h264_destroy(stm32n6_venc_h264_t *venc);

// Encode one NV12 frame. On success *out points to an Annex B access unit
// (SPS + PPS are prepended to IDR frames) which stays valid until the
// next call. The input planes must be clean in the D-cache. In streaming
// mode `y` and `uv` are the bases of the luma and chroma rings.
error_t stm32n6_venc_h264_encode(stm32n6_venc_h264_t *venc,
                                 const void *y, const void *uv,
                                 int force_idr,
                                 const uint8_t **out, size_t *outlen);

void stm32n6_venc_h264_set_qp(stm32n6_venc_h264_t *venc, int qp);

// Macroblock rows per slice, 0 = one slice per frame. Each slice is a
// separate NAL unit (and thus starts a new RTP packet), so a lost packet
// only damages its own slice. Takes effect from the next frame.
void stm32n6_venc_h264_set_slice_rows(stm32n6_venc_h264_t *venc, int rows);

void stm32n6_venc_h264_print_status(stm32n6_venc_h264_t *venc,
                                    struct stream *st);

// Print all non-zero encoder registers
void stm32n6_venc_dump_regs(struct stream *st);
