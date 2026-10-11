#pragma once

// STM32N6 VENC (VeriSilicon Hantro H1, "VC8000NanoE v5") registers.
//
// RM0486 only lists the registers as opaque 32 bit words. Field layout
// is from the VeriSilicon encoder package ST ships in STM32CubeN6
// (Middlewares/Third_Party/VideoEncoder, BSD-3-Clause), which targets
// this exact IP revision (SWREG0 = 0x6e655000).

#define VENC_BASE 0x58005000

#define VENC_SWREG(n) (VENC_BASE + (n) * 4)

#define VENC_ID_V5 0x6e655000

// SWREG1: interrupt status (write one to clear on this revision)
#define VENC_IRQ_LINE         (1 << 0)
#define VENC_IRQ_DISABLE      (1 << 1)
#define VENC_STATUS_FRAME_RDY (1 << 2)
#define VENC_STATUS_BUS_ERR   (1 << 3)
#define VENC_STATUS_RESET     (1 << 4)
#define VENC_STATUS_BUF_FULL  (1 << 5)
#define VENC_STATUS_TIMEOUT   (1 << 6)
#define VENC_STATUS_SLICE_RDY (1 << 8)
#define VENC_STATUS_ALL       0xf7c

// SWREG2: bus configuration for a little endian CPU and YUV input:
// all input/output swaps, burst length 16
#define VENC_SWREG2_DEFAULT   0x0000d00f

// SWREG3: MV output swaps, AXI dual channel disabled (as ST does for N6)
#define VENC_SWREG3_DEFAULT   0x0fd00000

// SWREG14: encoder control 0
#define VENC_CTRL_ENABLE          (1 << 0)
#define VENC_CTRL_MODE_JPEG       (2 << 1)
#define VENC_CTRL_MODE_H264       (3 << 1)
#define VENC_CTRL_PIC_INTER       (0 << 3)
#define VENC_CTRL_PIC_INTRA       (1 << 3)
#define VENC_CTRL_REC_WRITE_DIS   (1 << 6)
#define VENC_CTRL_REC_WRITE_BUF   (1 << 7)
#define VENC_CTRL_HEIGHT(mbs)     ((mbs) << 10)  // [18:10]
#define VENC_CTRL_WIDTH(mbs)      ((mbs) << 19)  // [27:19]
#define VENC_CTRL_INT_SLICE_RDY   (1 << 28)
#define VENC_CTRL_NAL_SIZE_WRITE  (1 << 29)
#define VENC_CTRL_MV_WRITE        (1 << 30)
#define VENC_CTRL_INT_TIMEOUT     (1u << 31)

// SWREG15: pre-processor
#define VENC_PP_ROT(x)            ((x) << 0)    // [1:0]
#define VENC_PP_FMT_I420          (0 << 2)      // [5:2]
#define VENC_PP_FMT_NV12          (1 << 2)
#define VENC_PP_YFILL(x)          ((x) << 6)    // [9:6]  pixels
#define VENC_PP_XFILL(x)          ((x) << 10)   // [11:10] 4 pixel units
#define VENC_PP_ROW_LENGTH(x)     ((x) << 12)   // [25:12] pixels
#define VENC_PP_LUM_OFFSET(x)     ((x) << 26)   // [28:26]
#define VENC_PP_CHR_OFFSET(x)     ((x) << 29)   // [31:29]

// SWREG16: H.264 PPS / slice header fields
#define VENC_H264_CONSTR_IP       (1 << 0)
#define VENC_H264_IDR_PIC_ID(x)   ((x) << 1)    // [4:1]
#define VENC_H264_CHROMA_QP(x)    (((x) & 0x1f) << 13) // [17:13]
#define VENC_H264_SLICE_BETA(x)   (((x) & 0xf) << 18)  // [21:18]
#define VENC_H264_SLICE_ALPHA(x)  (((x) & 0xf) << 22)  // [25:22]
#define VENC_H264_PIC_INIT_QP(x)  ((x) << 26)   // [31:26]

// SWREG17
#define VENC_H264_FRAME_NUM(x)    ((x) & 0xffff)
#define VENC_H264_PREV_MODE_FAVOR(x) ((x) << 16) // [23:16]
#define VENC_H264_PPS_ID(x)       ((x) << 24)

// SWREG18
#define VENC_H264_INTRA16_FAVOR(x) ((x) & 0xffff)
#define VENC_H264_STREAM_NAL      (1 << 16)     // 0 = Annex B byte stream
#define VENC_H264_INTER4_RESTRICT (1 << 17)
#define VENC_H264_CABAC_ENABLE    (1 << 18)
#define VENC_H264_TRANSFORM8X8    (1 << 21)
#define VENC_H264_DISABLE_QPEL    (1 << 22)
#define VENC_H264_SLICE_SIZE(x)   ((x) << 23)   // [29:23] MB rows, 0 = 1
#define VENC_H264_DEBLOCKING(x)   ((x) << 30)   // disable_deblocking_filter_idc

// SWREG19
#define VENC_DMV_PENALTY_1P(x)    ((x) << 0)    // [9:0]
#define VENC_DMV_PENALTY_4P(x)    ((x) << 10)   // [19:10]
#define VENC_DMV_PENALTY_QP(x)    ((x) << 20)   // [29:20]
#define VENC_SPLIT_MV             (1 << 30)
#define VENC_CHROMA_SWAP          (1u << 31)

// SWREG21
#define VENC_INTER_FAVOR(x)       ((x) & 0xffff)
#define VENC_SKIP_PENALTY(x)      ((x) << 24)

// SWREG27
#define VENC_QP_MIN(x)            ((x) << 14)   // [19:14]
#define VENC_QP_MAX(x)            ((x) << 20)   // [25:20]
#define VENC_QP(x)                ((x) << 26)   // [31:26]

// SWREG36
#define VENC_LONG_TERM_PENALTY(x) ((x) << 24)

// SWREG37
#define VENC_START_OFFSET(bits)   ((bits) << 23) // [28:23]

// SWREG38
#define VENC_MB_COUNT(x)          ((x) & 0xffff)

// SWREG56 / 60 / 61: area rectangles, disabled by placing them outside
// of the picture
#define VENC_AREA(l, r, t, b)     (((l) << 24) | ((r) << 16) | ((t) << 8) | (b))

// SWREG62
#define VENC_ROI2_DELTA_QP(x)     ((x) << 0)    // qp -= x inside ROI 2
#define VENC_ROI1_DELTA_QP(x)     ((x) << 4)    // qp -= x inside ROI 1
#define VENC_ZERO_MV_FAVOR(x)     ((x) << 28)

#define VENC_SWREG294_DEFAULT     0x00000034 // MB boost off (BoostQp 52)
#define VENC_SWREG295_DEFAULT     0x00013818 // Variance based intra/inter
#define VENC_SWREG422_DEFAULT     0x01020301 // Per channel AXI read IDs

// SWREG296: HW config 1 (read only)
#define VENC_CFG_IRQ_W1C          (1 << 23)

// SWREG497: low latency input (line buffer)
#define VENC_LL_MB_WR_PTR(x)      ((x) << 0)    // [8:0]
#define VENC_LL_SYNC_MB_ROWS(x)   ((x) << 18)   // [26:18]
#define VENC_LL_ENABLE            (1 << 27)
#define VENC_LL_LOOPBACK          (1 << 28)     // Input wraps around the ring
#define VENC_LL_HW_HANDSHAKE      (1 << 29)     // venc_rdy from the DCMIPP
#define VENC_LL_IRQ_ENABLE        (1 << 30)
