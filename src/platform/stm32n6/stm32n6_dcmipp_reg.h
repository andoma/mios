// STM32N6 DCMIPP (Digital camera interface pixel pipeline) register map
//
// Source: RM0486 Rev 2, chapter 39 (registers: 39.14, map: Table 366).
// Cross-checked against the ST CMSIS device header stm32n657xx.h
// (DCMIPP_TypeDef and bit definitions).
//
// Every address below is an absolute address (integer), suitable for
// reg_wr()/reg_rd() as used in src/platform/stm32n6.
//
// Conventions:
//   FOO_BAR          single-bit flag (already shifted)
//   FOO_BAZ(v)       multi-bit field, value v shifted into place
//   FOO_BAZ_MASK     mask of that field (in place)
//
// "UNSURE" marks items where the RM is ambiguous or self-contradictory.

#pragma once

/* ------------------------------------------------------------------------
 * Base address (RM0486 memory map, APB5)
 *   0x48002000 - 0x48002FFF  DCMIPP (nonsecure alias)
 *   0x58002000 - 0x58002FFF  DCMIPP (secure alias)
 *
 * Note: the alias used by the CPU must match the RIF configuration of the
 * DCMIPP RISUP (index 93). By default the RISUP is nonsecure, then the NS
 * alias is used and the DCMIPP AXI master writes are forced nonsecure
 * ("secure guard", RM 6.3.4) regardless of RIMC_ATTR9.MSEC.
 * If the RISUP is made secure (like the repo does for ETH1), use the
 * secure alias by defining DCMIPP_BASE before including this file.
 * ------------------------------------------------------------------------ */
#define DCMIPP_BASE_NS 0x48002000
#define DCMIPP_BASE_S  0x58002000
// The DCMIPP is made secure in the RIF (like ETH1), so use the secure
// alias
#define DCMIPP_BASE    DCMIPP_BASE_S

/* ------------------------------------------------------------------------
 * CSI-2 data type codes (as used in DTIDA/DTIDB/FDTF; same values as
 * DCMIPP_PRCR.FORMAT for the parallel port)
 * ------------------------------------------------------------------------ */
#define DCMIPP_DT_YUV420_8    0x18
#define DCMIPP_DT_YUV420_10   0x19
#define DCMIPP_DT_YUV422_8    0x1E
#define DCMIPP_DT_YUV422_10   0x1F
#define DCMIPP_DT_RGB444      0x20
#define DCMIPP_DT_RGB555      0x21
#define DCMIPP_DT_RGB565      0x22
#define DCMIPP_DT_RGB666      0x23
#define DCMIPP_DT_RGB888      0x24
#define DCMIPP_DT_RAW6        0x28
#define DCMIPP_DT_RAW7        0x29
#define DCMIPP_DT_RAW8        0x2A
#define DCMIPP_DT_RAW10       0x2B
#define DCMIPP_DT_RAW12       0x2C
#define DCMIPP_DT_RAW14       0x2D

/* ========================================================================
 * IP-Plug (AXI master) registers  0x000 - 0x06C
 * Writable only while locked (IPGR2.PSTART = 1 and IPGR3.IDLE = 1).
 * ======================================================================== */
#define DCMIPP_IPGR1          (DCMIPP_BASE + 0x000)  // reset 0x00000002
#define DCMIPP_IPGR2          (DCMIPP_BASE + 0x004)
#define DCMIPP_IPGR3          (DCMIPP_BASE + 0x008)  // reset 0x00000001
#define DCMIPP_IPGR8          (DCMIPP_BASE + 0x01C)  // reset 0xAA040314
// Client x = 1..5: 1=Pipe0, 2=Pipe1 Y/RGB, 3=Pipe1 U (or UV), 4=Pipe1 V, 5=Pipe2
#define DCMIPP_IPCxR1(x)      (DCMIPP_BASE + 0x020 + 0x10 * ((x) - 1))  // reset 0x00000004
#define DCMIPP_IPCxR2(x)      (DCMIPP_BASE + 0x024 + 0x10 * ((x) - 1))  // reset 0x00010000
#define DCMIPP_IPCxR3(x)      (DCMIPP_BASE + 0x028 + 0x10 * ((x) - 1))
#define DCMIPP_IPC1R1         DCMIPP_IPCxR1(1)
#define DCMIPP_IPC1R2         DCMIPP_IPCxR2(1)
#define DCMIPP_IPC1R3         DCMIPP_IPCxR3(1)  // reset 0x007F0000 (FIFO words   0..127)
#define DCMIPP_IPC2R1         DCMIPP_IPCxR1(2)
#define DCMIPP_IPC2R2         DCMIPP_IPCxR2(2)
#define DCMIPP_IPC2R3         DCMIPP_IPCxR3(2)  // reset 0x013F0080 (FIFO words 128..319)
#define DCMIPP_IPC3R1         DCMIPP_IPCxR1(3)
#define DCMIPP_IPC3R2         DCMIPP_IPCxR2(3)
#define DCMIPP_IPC3R3         DCMIPP_IPCxR3(3)  // reset 0x018F0140 (FIFO words 320..399)
#define DCMIPP_IPC4R1         DCMIPP_IPCxR1(4)
#define DCMIPP_IPC4R2         DCMIPP_IPCxR2(4)
#define DCMIPP_IPC4R3         DCMIPP_IPCxR3(4)  // reset 0x01BF0190 (FIFO words 400..447)
#define DCMIPP_IPC5R1         DCMIPP_IPCxR1(5)
#define DCMIPP_IPC5R2         DCMIPP_IPCxR2(5)
#define DCMIPP_IPC5R3         DCMIPP_IPCxR3(5)  // reset 0x027F01C0 (FIFO words 448..639)

#define DCMIPP_IPGR1_QOS_MODE          (1u << 24)
#define DCMIPP_IPGR1_MEMORYPAGE(v)     (((v) & 7u) << 0)   // 64 B << v (default 2 = 256 B)
#define DCMIPP_IPGR1_MEMORYPAGE_MASK   (7u << 0)
#define DCMIPP_IPGR2_PSTART            (1u << 0)           // 1 = request lock, 0 = run
#define DCMIPP_IPGR3_IDLE              (1u << 0)           // 1 = locked, may reconfigure
#define DCMIPP_IPGR8_IPPID_MASK        (0xFFu << 24)
#define DCMIPP_IPGR8_ARCHIID_MASK      (0x1Fu << 16)
#define DCMIPP_IPGR8_REVID_MASK        (0x1Fu << 8)
#define DCMIPP_IPGR8_DID_MASK          (0x3Fu << 0)
#define DCMIPP_IPCxR1_OTR(v)           (((v) & 0xFu) << 8) // 0 = no limit, n = n+1 outstanding
#define DCMIPP_IPCxR1_OTR_MASK         (0xFu << 8)
#define DCMIPP_IPCxR1_TRAFFIC(v)       (((v) & 7u) << 0)   // burst 8 B << v, 4 = 128 B (default)
#define DCMIPP_IPCxR1_TRAFFIC_MASK     (7u << 0)
#define DCMIPP_IPCxR2_WLRU(v)          (((v) & 0xFu) << 16) // ratio part = v + 1
#define DCMIPP_IPCxR2_WLRU_MASK        (0xFu << 16)
#define DCMIPP_IPCxR3_DPREGEND(v)      (((v) & 0x3FFu) << 16) // last 64-bit FIFO word (inclusive)
#define DCMIPP_IPCxR3_DPREGEND_MASK    (0x3FFu << 16)
#define DCMIPP_IPCxR3_DPREGSTART(v)    (((v) & 0x3FFu) << 0)  // total FIFO = 640 x 64-bit words
#define DCMIPP_IPCxR3_DPREGSTART_MASK  (0x3FFu << 0)

/* ========================================================================
 * Parallel interface 0x104 - 0x1FC (not used with CSI-2, listed for
 * completeness)
 * ======================================================================== */
#define DCMIPP_PRCR           (DCMIPP_BASE + 0x104)
#define DCMIPP_PRESCR         (DCMIPP_BASE + 0x108)
#define DCMIPP_PRESUR         (DCMIPP_BASE + 0x10C)
#define DCMIPP_PRIER          (DCMIPP_BASE + 0x1F4)
#define DCMIPP_PRSR           (DCMIPP_BASE + 0x1F8)  // reset 0x00030000
#define DCMIPP_PRFCR          (DCMIPP_BASE + 0x1FC)

#define DCMIPP_PRCR_SWAPBITS           (1u << 26)
#define DCMIPP_PRCR_SWAPCYCLES         (1u << 25)
#define DCMIPP_PRCR_FORMAT(v)          (((v) & 0xFFu) << 16) // 0x1E YUV422, 0x22 RGB565, 0x24 RGB888, 0x2A..0x2D RAW8..14, 0x4A..0x4D mono8..14, 0x5A byte stream
#define DCMIPP_PRCR_FORMAT_MASK        (0xFFu << 16)
#define DCMIPP_PRCR_ENABLE             (1u << 14)
#define DCMIPP_PRCR_EDM(v)             (((v) & 7u) << 10)    // 0:8 1:10 2:12 3:14 4:16 bits per pixel clock
#define DCMIPP_PRCR_EDM_MASK           (7u << 10)
#define DCMIPP_PRCR_VSPOL              (1u << 7)
#define DCMIPP_PRCR_HSPOL              (1u << 6)
#define DCMIPP_PRCR_PCKPOL             (1u << 5)
#define DCMIPP_PRCR_ESS                (1u << 4)
#define DCMIPP_PRESCR_FEC(v)           (((v) & 0xFFu) << 24)
#define DCMIPP_PRESCR_LEC(v)           (((v) & 0xFFu) << 16)
#define DCMIPP_PRESCR_LSC(v)           (((v) & 0xFFu) << 8)
#define DCMIPP_PRESCR_FSC(v)           (((v) & 0xFFu) << 0)
#define DCMIPP_PRESUR_FEU(v)           (((v) & 0xFFu) << 24)
#define DCMIPP_PRESUR_LEU(v)           (((v) & 0xFFu) << 16)
#define DCMIPP_PRESUR_LSU(v)           (((v) & 0xFFu) << 8)
#define DCMIPP_PRESUR_FSU(v)           (((v) & 0xFFu) << 0)
#define DCMIPP_PRIER_ERRIE             (1u << 6)
#define DCMIPP_PRSR_VSYNC              (1u << 17)
#define DCMIPP_PRSR_HSYNC              (1u << 16)
#define DCMIPP_PRSR_ERRF               (1u << 6)
#define DCMIPP_PRFCR_CERRF             (1u << 6)

/* ========================================================================
 * Common registers 0x204 - 0x3FC
 * ======================================================================== */
#define DCMIPP_CMCR           (DCMIPP_BASE + 0x204)
#define DCMIPP_CMFRCR         (DCMIPP_BASE + 0x208)
#define DCMIPP_CMIER          (DCMIPP_BASE + 0x3F0)
#define DCMIPP_CMSR1          (DCMIPP_BASE + 0x3F4)  // reset 0x00000003
#define DCMIPP_CMSR2          (DCMIPP_BASE + 0x3F8)
#define DCMIPP_CMFCR          (DCMIPP_BASE + 0x3FC)

#define DCMIPP_CMCR_SWAPRB             (1u << 7)  // swap R/U and B/V at input (pixel pipes only)
#define DCMIPP_CMCR_CFC                (1u << 4)  // write 1: clear frame counter (reads 0)
#define DCMIPP_CMCR_PSFC(v)            (((v) & 3u) << 1) // frame counter on pipe 0/1/2
#define DCMIPP_CMCR_PSFC_MASK          (3u << 1)
#define DCMIPP_CMCR_INSEL              (1u << 0)  // 0 = parallel, 1 = CSI-2

// CMIER / CMSR2 / CMFCR share bit positions
#define DCMIPP_CM_P2OVR                (1u << 31)
#define DCMIPP_CM_P2VSYNC              (1u << 26)
#define DCMIPP_CM_P2FRAME              (1u << 25)
#define DCMIPP_CM_P2LINE               (1u << 24)
#define DCMIPP_CM_P1OVR                (1u << 23)
#define DCMIPP_CM_P1VSYNC              (1u << 18)
#define DCMIPP_CM_P1FRAME              (1u << 17)
#define DCMIPP_CM_P1LINE               (1u << 16)
#define DCMIPP_CM_P0OVR                (1u << 15)
#define DCMIPP_CM_P0LIMIT              (1u << 14)
#define DCMIPP_CM_P0VSYNC              (1u << 10)
#define DCMIPP_CM_P0FRAME              (1u << 9)
#define DCMIPP_CM_P0LINE               (1u << 8)
#define DCMIPP_CM_PRERR                (1u << 6)
#define DCMIPP_CM_ATXERR               (1u << 5)
// RM names
#define DCMIPP_CMIER_P2OVRIE           DCMIPP_CM_P2OVR
#define DCMIPP_CMIER_P2VSYNCIE         DCMIPP_CM_P2VSYNC
#define DCMIPP_CMIER_P2FRAMEIE         DCMIPP_CM_P2FRAME
#define DCMIPP_CMIER_P2LINEIE          DCMIPP_CM_P2LINE
#define DCMIPP_CMIER_P1OVRIE           DCMIPP_CM_P1OVR
#define DCMIPP_CMIER_P1VSYNCIE         DCMIPP_CM_P1VSYNC
#define DCMIPP_CMIER_P1FRAMEIE         DCMIPP_CM_P1FRAME
#define DCMIPP_CMIER_P1LINEIE          DCMIPP_CM_P1LINE
#define DCMIPP_CMIER_P0OVRIE           DCMIPP_CM_P0OVR
#define DCMIPP_CMIER_P0LIMITIE         DCMIPP_CM_P0LIMIT
#define DCMIPP_CMIER_P0VSYNCIE         DCMIPP_CM_P0VSYNC
#define DCMIPP_CMIER_P0FRAMEIE         DCMIPP_CM_P0FRAME
#define DCMIPP_CMIER_P0LINEIE          DCMIPP_CM_P0LINE
#define DCMIPP_CMIER_PRERRIE           DCMIPP_CM_PRERR
#define DCMIPP_CMIER_ATXERRIE          DCMIPP_CM_ATXERR
#define DCMIPP_CMSR2_P2OVRF            DCMIPP_CM_P2OVR
#define DCMIPP_CMSR2_P2VSYNCF          DCMIPP_CM_P2VSYNC
#define DCMIPP_CMSR2_P2FRAMEF          DCMIPP_CM_P2FRAME
#define DCMIPP_CMSR2_P2LINEF           DCMIPP_CM_P2LINE
#define DCMIPP_CMSR2_P1OVRF            DCMIPP_CM_P1OVR
#define DCMIPP_CMSR2_P1VSYNCF          DCMIPP_CM_P1VSYNC
#define DCMIPP_CMSR2_P1FRAMEF          DCMIPP_CM_P1FRAME
#define DCMIPP_CMSR2_P1LINEF           DCMIPP_CM_P1LINE
#define DCMIPP_CMSR2_P0OVRF            DCMIPP_CM_P0OVR
#define DCMIPP_CMSR2_P0LIMITF          DCMIPP_CM_P0LIMIT
#define DCMIPP_CMSR2_P0VSYNCF          DCMIPP_CM_P0VSYNC
#define DCMIPP_CMSR2_P0FRAMEF          DCMIPP_CM_P0FRAME
#define DCMIPP_CMSR2_P0LINEF           DCMIPP_CM_P0LINE
#define DCMIPP_CMSR2_PRERRF            DCMIPP_CM_PRERR
#define DCMIPP_CMSR2_ATXERRF           DCMIPP_CM_ATXERR
#define DCMIPP_CMFCR_CP2OVRF           DCMIPP_CM_P2OVR
#define DCMIPP_CMFCR_CP2VSYNCF         DCMIPP_CM_P2VSYNC
#define DCMIPP_CMFCR_CP2FRAMEF         DCMIPP_CM_P2FRAME
#define DCMIPP_CMFCR_CP2LINEF          DCMIPP_CM_P2LINE
#define DCMIPP_CMFCR_CP1OVRF           DCMIPP_CM_P1OVR
#define DCMIPP_CMFCR_CP1VSYNCF         DCMIPP_CM_P1VSYNC
#define DCMIPP_CMFCR_CP1FRAMEF         DCMIPP_CM_P1FRAME
#define DCMIPP_CMFCR_CP1LINEF          DCMIPP_CM_P1LINE
#define DCMIPP_CMFCR_CP0OVRF           DCMIPP_CM_P0OVR
#define DCMIPP_CMFCR_CP0LIMITF         DCMIPP_CM_P0LIMIT
#define DCMIPP_CMFCR_CP0VSYNCF         DCMIPP_CM_P0VSYNC
#define DCMIPP_CMFCR_CP0FRAMEF         DCMIPP_CM_P0FRAME
#define DCMIPP_CMFCR_CP0LINEF          DCMIPP_CM_P0LINE
#define DCMIPP_CMFCR_CPRERRF           DCMIPP_CM_PRERR
#define DCMIPP_CMFCR_CATXERRF          DCMIPP_CM_ATXERR

#define DCMIPP_CMSR1_P2CPTACT          (1u << 31)
#define DCMIPP_CMSR1_P2LSTFRM          (1u << 25)
#define DCMIPP_CMSR1_P2LSTLINE         (1u << 24)
#define DCMIPP_CMSR1_P1CPTACT          (1u << 23)
#define DCMIPP_CMSR1_P1LSTFRM          (1u << 17)
#define DCMIPP_CMSR1_P1LSTLINE         (1u << 16)
#define DCMIPP_CMSR1_P0CPTACT          (1u << 15)
#define DCMIPP_CMSR1_P0LSTFRM          (1u << 9)
#define DCMIPP_CMSR1_P0LSTLINE         (1u << 8)
#define DCMIPP_CMSR1_PRVSYNC           (1u << 1)
#define DCMIPP_CMSR1_PRHSYNC           (1u << 0)

/* ========================================================================
 * Generic per-pipe registers, p = pipe number 0, 1, 2.
 * (The RM's "Pipex ... 0x900 + 0x400 * (x - 1)" for x = 1..2 is the same
 * as these with p = x; Pipe0 follows the same stride.)
 * Physical ("current") copy of a shadowed register = shadow + 0x200.
 * ======================================================================== */
#define DCMIPP_PxFSCR(p)      (DCMIPP_BASE + 0x404 + 0x400 * (p))  // flow selection
#define DCMIPP_PxFCTCR(p)     (DCMIPP_BASE + 0x500 + 0x400 * (p))  // flow control
#define DCMIPP_PxPPCR(p)      (DCMIPP_BASE + 0x5C0 + 0x400 * (p))  // pixel packer config
#define DCMIPP_PxPPM0AR1(p)   (DCMIPP_BASE + 0x5C4 + 0x400 * (p))
#define DCMIPP_PxPPM0AR2(p)   (DCMIPP_BASE + 0x5C8 + 0x400 * (p))
#define DCMIPP_PxSTM0AR(p)    (DCMIPP_BASE + 0x5D0 + 0x400 * (p))
#define DCMIPP_PxIER(p)       (DCMIPP_BASE + 0x5F4 + 0x400 * (p))
#define DCMIPP_PxSR(p)        (DCMIPP_BASE + 0x5F8 + 0x400 * (p))
#define DCMIPP_PxFCR(p)       (DCMIPP_BASE + 0x5FC + 0x400 * (p))
#define DCMIPP_PxCFSCR(p)     (DCMIPP_PxFSCR(p) + 0x200)
#define DCMIPP_PxCFCTCR(p)    (DCMIPP_PxFCTCR(p) + 0x200)
#define DCMIPP_PxCPPCR(p)     (DCMIPP_PxPPCR(p) + 0x200)
#define DCMIPP_PxCPPM0AR1(p)  (DCMIPP_PxPPM0AR1(p) + 0x200)
#define DCMIPP_PxCPPM0AR2(p)  (DCMIPP_PxPPM0AR2(p) + 0x200)
// Pixel pipes only (p = 1, 2)
#define DCMIPP_PxCRSTR(p)     (DCMIPP_BASE + 0x504 + 0x400 * (p))  // crop start
#define DCMIPP_PxCRSZR(p)     (DCMIPP_BASE + 0x508 + 0x400 * (p))  // crop size
#define DCMIPP_PxDCCR(p)      (DCMIPP_BASE + 0x50C + 0x400 * (p))  // decimation (pre-downsize)
#define DCMIPP_PxDSCR(p)      (DCMIPP_BASE + 0x510 + 0x400 * (p))  // downsize config
#define DCMIPP_PxDSRTIOR(p)   (DCMIPP_BASE + 0x514 + 0x400 * (p))  // downsize ratio
#define DCMIPP_PxDSSZR(p)     (DCMIPP_BASE + 0x518 + 0x400 * (p))  // downsize dest size
#define DCMIPP_PxCMRICR(p)    (DCMIPP_BASE + 0x520 + 0x400 * (p))  // common ROI config
#define DCMIPP_PxRIyCR1(p, y) (DCMIPP_BASE + 0x524 + 0x400 * (p) + 8 * ((y) - 1)) // y = 1..8
#define DCMIPP_PxRIyCR2(p, y) (DCMIPP_BASE + 0x528 + 0x400 * (p) + 8 * ((y) - 1))
#define DCMIPP_PxGMCR(p)      (DCMIPP_BASE + 0x570 + 0x400 * (p))  // gamma
#define DCMIPP_PxPPM0PR(p)    (DCMIPP_BASE + 0x5CC + 0x400 * (p))  // Memory0 pitch
#define DCMIPP_PxCCRSTR(p)    (DCMIPP_PxCRSTR(p) + 0x200)
#define DCMIPP_PxCCRSZR(p)    (DCMIPP_PxCRSZR(p) + 0x200)
#define DCMIPP_PxCDCCR(p)     (DCMIPP_PxDCCR(p) + 0x200)
#define DCMIPP_PxCDSCR(p)     (DCMIPP_PxDSCR(p) + 0x200)
#define DCMIPP_PxCDSRTIOR(p)  (DCMIPP_PxDSRTIOR(p) + 0x200)
#define DCMIPP_PxCDSSZR(p)    (DCMIPP_PxDSSZR(p) + 0x200)
#define DCMIPP_PxCCMRICR(p)   (DCMIPP_PxCMRICR(p) + 0x200)
#define DCMIPP_PxCRIyCR1(p, y) (DCMIPP_PxRIyCR1(p, y) + 0x200)
#define DCMIPP_PxCRIyCR2(p, y) (DCMIPP_PxRIyCR2(p, y) + 0x200)
#define DCMIPP_PxCPPM0PR(p)   (DCMIPP_PxPPM0PR(p) + 0x200)

/* ========================================================================
 * Pipe0 (dump pipe)
 * ======================================================================== */
#define DCMIPP_P0FSCR         (DCMIPP_BASE + 0x404)
#define DCMIPP_P0FCTCR        (DCMIPP_BASE + 0x500)
#define DCMIPP_P0SCSTR        (DCMIPP_BASE + 0x504)
#define DCMIPP_P0SCSZR        (DCMIPP_BASE + 0x508)
#define DCMIPP_P0DCCNTR       (DCMIPP_BASE + 0x5B0)
#define DCMIPP_P0DCLMTR       (DCMIPP_BASE + 0x5B4)  // reset 0x00FFFFFF
#define DCMIPP_P0PPCR         (DCMIPP_BASE + 0x5C0)
#define DCMIPP_P0PPM0AR1      (DCMIPP_BASE + 0x5C4)
#define DCMIPP_P0PPM0AR2      (DCMIPP_BASE + 0x5C8)
#define DCMIPP_P0STM0AR       (DCMIPP_BASE + 0x5D0)
#define DCMIPP_P0IER          (DCMIPP_BASE + 0x5F4)
#define DCMIPP_P0SR           (DCMIPP_BASE + 0x5F8)
#define DCMIPP_P0FCR          (DCMIPP_BASE + 0x5FC)
#define DCMIPP_P0CFSCR        (DCMIPP_BASE + 0x604)
#define DCMIPP_P0CFCTCR       (DCMIPP_BASE + 0x700)
#define DCMIPP_P0CSCSTR       (DCMIPP_BASE + 0x704)
#define DCMIPP_P0CSCSZR       (DCMIPP_BASE + 0x708)
#define DCMIPP_P0CPPCR        (DCMIPP_BASE + 0x7C0)
#define DCMIPP_P0CPPM0AR1     (DCMIPP_BASE + 0x7C4)
#define DCMIPP_P0CPPM0AR2     (DCMIPP_BASE + 0x7C8)
// Note: Pipe0 has no pitch register; lines are written back to back.

/* ========================================================================
 * Pipe1 ISP (shadow 0x800.., physical 0xA00..)
 * ======================================================================== */
#define DCMIPP_P1FSCR         (DCMIPP_BASE + 0x804)
#define DCMIPP_P1SRCR         (DCMIPP_BASE + 0x820)  // stat removal (not shadowed)
#define DCMIPP_P1BPRCR        (DCMIPP_BASE + 0x824)  // bad pixel removal
#define DCMIPP_P1BPRSR        (DCMIPP_BASE + 0x828)
#define DCMIPP_P1DECR         (DCMIPP_BASE + 0x830)  // ISP input decimation (not shadowed)
#define DCMIPP_P1BLCCR        (DCMIPP_BASE + 0x840)  // black level
#define DCMIPP_P1EXCR1        (DCMIPP_BASE + 0x844)  // exposure / WB red
#define DCMIPP_P1EXCR2        (DCMIPP_BASE + 0x848)  // exposure / WB green, blue
#define DCMIPP_P1ST1CR        (DCMIPP_BASE + 0x850)
#define DCMIPP_P1ST2CR        (DCMIPP_BASE + 0x854)
#define DCMIPP_P1ST3CR        (DCMIPP_BASE + 0x858)
#define DCMIPP_P1STyCR(y)     (DCMIPP_BASE + 0x850 + 4 * ((y) - 1))  // y = 1..3
#define DCMIPP_P1STSTR        (DCMIPP_BASE + 0x85C)
#define DCMIPP_P1STSZR        (DCMIPP_BASE + 0x860)
#define DCMIPP_P1ST1SR        (DCMIPP_BASE + 0x864)
#define DCMIPP_P1ST2SR        (DCMIPP_BASE + 0x868)
#define DCMIPP_P1ST3SR        (DCMIPP_BASE + 0x86C)
#define DCMIPP_P1STySR(y)     (DCMIPP_BASE + 0x864 + 4 * ((y) - 1))  // y = 1..3
#define DCMIPP_P1DMCR         (DCMIPP_BASE + 0x870)  // demosaicing (not shadowed)
#define DCMIPP_P1CCCR         (DCMIPP_BASE + 0x880)  // color conversion (CCM)
#define DCMIPP_P1CCRR1        (DCMIPP_BASE + 0x884)
#define DCMIPP_P1CCRR2        (DCMIPP_BASE + 0x888)
#define DCMIPP_P1CCGR1        (DCMIPP_BASE + 0x88C)
#define DCMIPP_P1CCGR2        (DCMIPP_BASE + 0x890)
#define DCMIPP_P1CCBR1        (DCMIPP_BASE + 0x894)
#define DCMIPP_P1CCBR2        (DCMIPP_BASE + 0x898)
#define DCMIPP_P1CTCR1        (DCMIPP_BASE + 0x8A0)  // contrast, reset 0x00002000
#define DCMIPP_P1CTCR2        (DCMIPP_BASE + 0x8A4)  // reset 0x20202020
#define DCMIPP_P1CTCR3        (DCMIPP_BASE + 0x8A8)  // reset 0x20202020

/* ========================================================================
 * Pipe1 post-processing (shadow 0x900.., physical 0xB00..)
 * ======================================================================== */
#define DCMIPP_P1FCTCR        (DCMIPP_BASE + 0x900)
#define DCMIPP_P1CRSTR        (DCMIPP_BASE + 0x904)
#define DCMIPP_P1CRSZR        (DCMIPP_BASE + 0x908)
#define DCMIPP_P1DCCR         (DCMIPP_BASE + 0x90C)
#define DCMIPP_P1DSCR         (DCMIPP_BASE + 0x910)
#define DCMIPP_P1DSRTIOR      (DCMIPP_BASE + 0x914)
#define DCMIPP_P1DSSZR        (DCMIPP_BASE + 0x918)
#define DCMIPP_P1CMRICR       (DCMIPP_BASE + 0x920)
#define DCMIPP_P1RIxCR1(x)    (DCMIPP_BASE + 0x924 + 8 * ((x) - 1))  // x = 1..8
#define DCMIPP_P1RIxCR2(x)    (DCMIPP_BASE + 0x928 + 8 * ((x) - 1))
#define DCMIPP_P1GMCR         (DCMIPP_BASE + 0x970)
#define DCMIPP_P1YUVCR        (DCMIPP_BASE + 0x980)  // RGB->YUV conversion
#define DCMIPP_P1YUVRR1       (DCMIPP_BASE + 0x984)
#define DCMIPP_P1YUVRR2       (DCMIPP_BASE + 0x988)
#define DCMIPP_P1YUVGR1       (DCMIPP_BASE + 0x98C)
#define DCMIPP_P1YUVGR2       (DCMIPP_BASE + 0x990)
#define DCMIPP_P1YUVBR1       (DCMIPP_BASE + 0x994)
#define DCMIPP_P1YUVBR2       (DCMIPP_BASE + 0x998)
#define DCMIPP_P1PPCR         (DCMIPP_BASE + 0x9C0)
#define DCMIPP_P1PPM0AR1      (DCMIPP_BASE + 0x9C4)  // Y (or RGB) plane, buffer 1
#define DCMIPP_P1PPM0AR2      (DCMIPP_BASE + 0x9C8)  // Y plane, buffer 2 (DBM)
#define DCMIPP_P1PPM0PR       (DCMIPP_BASE + 0x9CC)  // Y pitch
#define DCMIPP_P1STM0AR       (DCMIPP_BASE + 0x9D0)  // status: M0 address of last captured frame
#define DCMIPP_P1PPM1AR1      (DCMIPP_BASE + 0x9D4)  // UV (or U) plane, buffer 1
#define DCMIPP_P1PPM1AR2      (DCMIPP_BASE + 0x9D8)  // UV plane, buffer 2 (DBM)
#define DCMIPP_P1PPM1PR       (DCMIPP_BASE + 0x9DC)  // UV pitch (also used for M2)
#define DCMIPP_P1STM1AR       (DCMIPP_BASE + 0x9E0)
#define DCMIPP_P1PPM2AR1      (DCMIPP_BASE + 0x9E4)  // V plane (YUV420-3 only)
#define DCMIPP_P1PPM2AR2      (DCMIPP_BASE + 0x9E8)
#define DCMIPP_P1STM2AR       (DCMIPP_BASE + 0x9F0)
#define DCMIPP_P1IER          (DCMIPP_BASE + 0x9F4)
#define DCMIPP_P1SR           (DCMIPP_BASE + 0x9F8)
#define DCMIPP_P1FCR          (DCMIPP_BASE + 0x9FC)
// RM generic names for the Memory1/2 registers (x = 1 only)
#define DCMIPP_PxPPM1AR1(x)   DCMIPP_P1PPM1AR1
#define DCMIPP_PxPPM1AR2(x)   DCMIPP_P1PPM1AR2
#define DCMIPP_PxPPM1PR(x)    DCMIPP_P1PPM1PR
#define DCMIPP_PxSTM1AR(x)    DCMIPP_P1STM1AR
#define DCMIPP_PxPPM2AR1(x)   DCMIPP_P1PPM2AR1
#define DCMIPP_PxPPM2AR2(x)   DCMIPP_P1PPM2AR2
#define DCMIPP_PxSTM2AR(x)    DCMIPP_P1STM2AR

/* Pipe1 physical ("current") registers, read-only */
#define DCMIPP_P1CFSCR        (DCMIPP_BASE + 0xA04)
#define DCMIPP_P1CBPRCR       (DCMIPP_BASE + 0xA24)
#define DCMIPP_P1CBLCCR       (DCMIPP_BASE + 0xA40)
#define DCMIPP_P1CEXCR1       (DCMIPP_BASE + 0xA44)
#define DCMIPP_P1CEXCR2       (DCMIPP_BASE + 0xA48)
#define DCMIPP_P1CST1CR       (DCMIPP_BASE + 0xA50)  // also holds ACCU[23:0] in bits 31:8
#define DCMIPP_P1CST2CR       (DCMIPP_BASE + 0xA54)
#define DCMIPP_P1CST3CR       (DCMIPP_BASE + 0xA58)
#define DCMIPP_P1CSTSTR       (DCMIPP_BASE + 0xA5C)
#define DCMIPP_P1CSTSZR       (DCMIPP_BASE + 0xA60)
#define DCMIPP_P1CCCCR        (DCMIPP_BASE + 0xA80)
#define DCMIPP_P1CCCRR1       (DCMIPP_BASE + 0xA84)
#define DCMIPP_P1CCCRR2       (DCMIPP_BASE + 0xA88)
#define DCMIPP_P1CCCGR1       (DCMIPP_BASE + 0xA8C)
#define DCMIPP_P1CCCGR2       (DCMIPP_BASE + 0xA90)
#define DCMIPP_P1CCCBR1       (DCMIPP_BASE + 0xA94)
#define DCMIPP_P1CCCBR2       (DCMIPP_BASE + 0xA98)
#define DCMIPP_P1CCTCR1       (DCMIPP_BASE + 0xAA0)
#define DCMIPP_P1CCTCR2       (DCMIPP_BASE + 0xAA4)
#define DCMIPP_P1CCTCR3       (DCMIPP_BASE + 0xAA8)
#define DCMIPP_P1CFCTCR       (DCMIPP_BASE + 0xB00)
#define DCMIPP_P1CCRSTR       (DCMIPP_BASE + 0xB04)
#define DCMIPP_P1CCRSZR       (DCMIPP_BASE + 0xB08)
#define DCMIPP_P1CDCCR        (DCMIPP_BASE + 0xB0C)
#define DCMIPP_P1CDSCR        (DCMIPP_BASE + 0xB10)
#define DCMIPP_P1CDSRTIOR     (DCMIPP_BASE + 0xB14)
#define DCMIPP_P1CDSSZR       (DCMIPP_BASE + 0xB18)
#define DCMIPP_P1CCMRICR      (DCMIPP_BASE + 0xB20)
#define DCMIPP_P1CRIxCR1(x)   (DCMIPP_BASE + 0xB24 + 8 * ((x) - 1))
#define DCMIPP_P1CRIxCR2(x)   (DCMIPP_BASE + 0xB28 + 8 * ((x) - 1))
#define DCMIPP_P1CPPCR        (DCMIPP_BASE + 0xBC0)
#define DCMIPP_P1CPPM0AR1     (DCMIPP_BASE + 0xBC4)
#define DCMIPP_P1CPPM0AR2     (DCMIPP_BASE + 0xBC8)
#define DCMIPP_P1CPPM0PR      (DCMIPP_BASE + 0xBCC)
#define DCMIPP_P1CPPM1AR1     (DCMIPP_BASE + 0xBD4)
#define DCMIPP_P1CPPM1AR2     (DCMIPP_BASE + 0xBD8)
#define DCMIPP_P1CPPM1PR      (DCMIPP_BASE + 0xBDC)
#define DCMIPP_P1CPPM2AR1     (DCMIPP_BASE + 0xBE4)
#define DCMIPP_P1CPPM2AR2     (DCMIPP_BASE + 0xBE8)

/* ========================================================================
 * Pipe2 (ancillary pipe)
 * ======================================================================== */
#define DCMIPP_P2FSCR         (DCMIPP_BASE + 0xC04)
#define DCMIPP_P2FCTCR        (DCMIPP_BASE + 0xD00)
#define DCMIPP_P2CRSTR        (DCMIPP_BASE + 0xD04)
#define DCMIPP_P2CRSZR        (DCMIPP_BASE + 0xD08)
#define DCMIPP_P2DCCR         (DCMIPP_BASE + 0xD0C)
#define DCMIPP_P2DSCR         (DCMIPP_BASE + 0xD10)
#define DCMIPP_P2DSRTIOR      (DCMIPP_BASE + 0xD14)
#define DCMIPP_P2DSSZR        (DCMIPP_BASE + 0xD18)
#define DCMIPP_P2CMRICR       (DCMIPP_BASE + 0xD20)
#define DCMIPP_P2RIxCR1(x)    (DCMIPP_BASE + 0xD24 + 8 * ((x) - 1))  // x = 1..8
#define DCMIPP_P2RIxCR2(x)    (DCMIPP_BASE + 0xD28 + 8 * ((x) - 1))
#define DCMIPP_P2GMCR         (DCMIPP_BASE + 0xD70)
#define DCMIPP_P2PPCR         (DCMIPP_BASE + 0xDC0)
#define DCMIPP_P2PPM0AR1      (DCMIPP_BASE + 0xDC4)
#define DCMIPP_P2PPM0AR2      (DCMIPP_BASE + 0xDC8)
#define DCMIPP_P2PPM0PR       (DCMIPP_BASE + 0xDCC)
#define DCMIPP_P2STM0AR       (DCMIPP_BASE + 0xDD0)
#define DCMIPP_P2IER          (DCMIPP_BASE + 0xDF4)
#define DCMIPP_P2SR           (DCMIPP_BASE + 0xDF8)
#define DCMIPP_P2FCR          (DCMIPP_BASE + 0xDFC)
#define DCMIPP_P2CFSCR        (DCMIPP_BASE + 0xE04)
#define DCMIPP_P2CFCTCR       (DCMIPP_BASE + 0xF00)
#define DCMIPP_P2CCRSTR       (DCMIPP_BASE + 0xF04)
#define DCMIPP_P2CCRSZR       (DCMIPP_BASE + 0xF08)
#define DCMIPP_P2CDCCR        (DCMIPP_BASE + 0xF0C)
#define DCMIPP_P2CDSCR        (DCMIPP_BASE + 0xF10)
#define DCMIPP_P2CDSRTIOR     (DCMIPP_BASE + 0xF14)
#define DCMIPP_P2CDSSZR       (DCMIPP_BASE + 0xF18)
#define DCMIPP_P2CCMRICR      (DCMIPP_BASE + 0xF20)  // UNSURE: from RM formula 0xB20+0x400; CMSIS marks 0xF20 reserved
#define DCMIPP_P2CRIxCR1(x)   (DCMIPP_BASE + 0xF24 + 8 * ((x) - 1))
#define DCMIPP_P2CRIxCR2(x)   (DCMIPP_BASE + 0xF28 + 8 * ((x) - 1))
#define DCMIPP_P2CPPCR        (DCMIPP_BASE + 0xFC0)
#define DCMIPP_P2CPPM0AR1     (DCMIPP_BASE + 0xFC4)
#define DCMIPP_P2CPPM0AR2     (DCMIPP_BASE + 0xFC8)
#define DCMIPP_P2CPPM0PR      (DCMIPP_BASE + 0xFCC)  // from RM formula and CMSIS; not in Table 366

/* ID / HW config registers (not described in RM0486 Rev 2, only in CMSIS) */
#define DCMIPP_HWCFGR2        (DCMIPP_BASE + 0xFEC)
#define DCMIPP_HWCFGR1        (DCMIPP_BASE + 0xFF0)
#define DCMIPP_VERR           (DCMIPP_BASE + 0xFF4)
#define DCMIPP_IPIDR          (DCMIPP_BASE + 0xFF8)
#define DCMIPP_SIDR           (DCMIPP_BASE + 0xFFC)

/* ========================================================================
 * Bit fields
 * ======================================================================== */

/* ---- PxFSCR: flow selection (P0FSCR, P1FSCR, P2FSCR) ---- */
#define DCMIPP_FSCR_PIPEN              (1u << 31)  // NOT shadowed (takes effect immediately)
#define DCMIPP_FSCR_FDTFEN             (1u << 30)  // P1/P2 only
#define DCMIPP_FSCR_FDTF(v)            (((v) & 0x3Fu) << 24) // P1/P2 only
#define DCMIPP_FSCR_FDTF_MASK          (0x3Fu << 24)
#define DCMIPP_FSCR_VC(v)              (((v) & 3u) << 19)
#define DCMIPP_FSCR_VC_MASK            (3u << 19)
#define DCMIPP_FSCR_PIPEDIFF           (1u << 18)  // P1FSCR only
#define DCMIPP_FSCR_DTMODE(v)          (((v) & 3u) << 16)  // P0/P1 only
#define DCMIPP_FSCR_DTMODE_MASK        (3u << 16)
#define DCMIPP_DTMODE_DTIDA            0  // only DTIDA
#define DCMIPP_DTMODE_DTIDA_OR_DTIDB   1  // DTIDA and/or DTIDB (P1: valid only with FDTFEN, per P1CFSCR doc)
#define DCMIPP_DTMODE_ALL_EXCEPT_AB    2  // P0 only
#define DCMIPP_DTMODE_ALL              3  // P0 only
#define DCMIPP_FSCR_DTIDB(v)           (((v) & 0x3Fu) << 8)  // P0/P1 only
#define DCMIPP_FSCR_DTIDB_MASK         (0x3Fu << 8)
#define DCMIPP_FSCR_DTIDA(v)           (((v) & 0x3Fu) << 0)
#define DCMIPP_FSCR_DTIDA_MASK         (0x3Fu << 0)

/* ---- PxFCTCR: flow control (P0FCTCR 0x500, P1FCTCR 0x900, P2FCTCR 0xD00) ---- */
#define DCMIPP_FCTCR_CPTREQ            (1u << 3)
#define DCMIPP_FCTCR_CPTMODE           (1u << 2)   // 0 = continuous, 1 = snapshot
#define DCMIPP_FCTCR_FRATE(v)          (((v) & 3u) << 0) // 0 all, 1 = 1/2, 2 = 1/4, 3 = 1/8 (continuous only)
#define DCMIPP_FCTCR_FRATE_MASK        (3u << 0)

/* ---- PxIER / PxSR / PxFCR (same bit positions for all pipes) ---- */
#define DCMIPP_PIER_OVRIE              (1u << 7)
#define DCMIPP_PIER_LIMITIE            (1u << 6)   // P0 only
#define DCMIPP_PIER_VSYNCIE            (1u << 2)
#define DCMIPP_PIER_FRAMEIE            (1u << 1)
#define DCMIPP_PIER_LINEIE             (1u << 0)
#define DCMIPP_PSR_CPTACT              (1u << 23)
#define DCMIPP_PSR_LSTFRM              (1u << 17)
#define DCMIPP_PSR_LSTLINE             (1u << 16)
#define DCMIPP_PSR_OVRF                (1u << 7)
#define DCMIPP_PSR_LIMITF              (1u << 6)   // P0 only
#define DCMIPP_PSR_VSYNCF              (1u << 2)
#define DCMIPP_PSR_FRAMEF              (1u << 1)
#define DCMIPP_PSR_LINEF               (1u << 0)
#define DCMIPP_PFCR_COVRF              (1u << 7)
#define DCMIPP_PFCR_CLIMITF            (1u << 6)   // P0 only
#define DCMIPP_PFCR_CVSYNCF            (1u << 2)
#define DCMIPP_PFCR_CFRAMEF            (1u << 1)
#define DCMIPP_PFCR_CLINEF             (1u << 0)

/* ---- P0SCSTR / P0SCSZR: Pipe0 crop (horizontal unit = 32-bit word) ---- */
#define DCMIPP_P0SCSTR_VSTART(v)       (((v) & 0xFFFu) << 16) // lines
#define DCMIPP_P0SCSTR_HSTART(v)       (((v) & 0xFFFu) << 0)  // 32-bit words
#define DCMIPP_P0SCSZR_ENABLE          (1u << 31)
#define DCMIPP_P0SCSZR_POSNEG          (1u << 30)  // 1 = keep outside of the window
#define DCMIPP_P0SCSZR_VSIZE(v)        (((v) & 0xFFFu) << 16) // lines (0 = no vertical crop)
#define DCMIPP_P0SCSZR_HSIZE(v)        (((v) & 0xFFFu) << 0)  // 32-bit words (0 = no horiz crop)

/* ---- P0DCCNTR / P0DCLMTR ---- */
#define DCMIPP_P0DCCNTR_CNT_MASK       (0x3FFFFFFu << 0)  // bytes (4-byte granularity for CSI)
#define DCMIPP_P0DCLMTR_ENABLE         (1u << 31)
#define DCMIPP_P0DCLMTR_LIMIT(v)       (((v) & 0xFFFFFFu) << 0) // in 32-bit words
#define DCMIPP_P0DCLMTR_LIMIT_MASK     (0xFFFFFFu << 0)

/* ---- P0PPCR: Pipe0 pixel packer ---- */
#define DCMIPP_P0PPCR_LMAWE            (1u << 20)  // UNSURE: documented only in P0CPPCR (current), absent from P0PPCR and CMSIS
#define DCMIPP_P0PPCR_LMAWM(v)         (((v) & 7u) << 17) // UNSURE: same as above
#define DCMIPP_P0PPCR_DBM              (1u << 16)
#define DCMIPP_P0PPCR_LINEMULT(v)      (((v) & 7u) << 13) // LINE event every 2^v lines
#define DCMIPP_P0PPCR_LINEMULT_MASK    (7u << 13)
#define DCMIPP_P0PPCR_OELS             (1u << 11)  // with LSM: 0 keep 1st line, 1 keep 2nd line
#define DCMIPP_P0PPCR_LSM              (1u << 10)  // capture one line out of two
#define DCMIPP_P0PPCR_OEBS             (1u << 9)   // parallel only
#define DCMIPP_P0PPCR_BSM(v)           (((v) & 3u) << 7) // parallel only
#define DCMIPP_P0PPCR_HEADEREN         (1u << 6)   // dump CSI-2 headers as 32-bit words
#define DCMIPP_P0PPCR_PAD              (1u << 5)   // RAW10/12/14 on 16 bit: 0 = LSB aligned, 1 = MSB aligned
#define DCMIPP_P0PPCR_SWAPYUV          (1u << 0)   // swap bytes 0<->1 and 2<->3

/* ---- P1PPCR / P2PPCR: pixel pipe packer ---- */
#define DCMIPP_PPCR_LMAWE              (1u << 20)  // line multi address wrapping enable
#define DCMIPP_PPCR_LMAWM(v)           (((v) & 7u) << 17) // wrap to base after 2^v lines
#define DCMIPP_PPCR_LMAWM_MASK         (7u << 17)
#define DCMIPP_PPCR_DBM                (1u << 16)  // double buffer (M0AR1/M0AR2, M1AR1/M1AR2, M2AR1/M2AR2)
#define DCMIPP_PPCR_LINEMULT(v)        (((v) & 7u) << 13) // LINE event every 2^v lines (0 = 1 .. 7 = 128)
#define DCMIPP_PPCR_LINEMULT_MASK      (7u << 13)
#define DCMIPP_PPCR_SWAPRB             (1u << 4)   // swap R/B (U/V)
#define DCMIPP_PPCR_FORMAT(v)          (((v) & 0xFu) << 0)
#define DCMIPP_PPCR_FORMAT_MASK        (0xFu << 0)
#define DCMIPP_PPFMT_RGB888            0x0  // RGB888 / YUV444 1-buffer (24 bpp)
#define DCMIPP_PPFMT_RGB565            0x1
#define DCMIPP_PPFMT_ARGB8888          0x2  // A = 0xFF
#define DCMIPP_PPFMT_RGBA8888          0x3  // A = 0xFF
#define DCMIPP_PPFMT_Y8                0x4  // mono Y8 / G8
#define DCMIPP_PPFMT_AYUV              0x5  // YUV444 32 bpp
#define DCMIPP_PPFMT_YUYV              0x6  // YUV422-1
#define DCMIPP_PPFMT_YUV422_2          0x7  // P1 only, Y + UV planes
#define DCMIPP_PPFMT_YUV420_2          0x8  // P1 only, Y + UV planes. RM: "NV21, NV12 with SWAPRB=1" (UNSURE, see notes)
#define DCMIPP_PPFMT_YUV420_3          0x9  // P1 only, YV12 (Y, U, V planes)
#define DCMIPP_PPFMT_UYVY              0xA  // P1 only

#define DCMIPP_PPMxAR_MASK             0xFFFFFFF0u  // addresses: bits 3:0 read-only 0 (16-byte aligned)
#define DCMIPP_PPMxPR_PITCH(v)         ((v) & 0x7FF0u) // bytes, multiple of 16, max 32767

/* ---- P1SRCR: statistics (line) removal ---- */
#define DCMIPP_P1SRCR_CROPEN           (1u << 15)
#define DCMIPP_P1SRCR_FIRSTLINEDEL(v)  (((v) & 7u) << 12)
#define DCMIPP_P1SRCR_LASTLINE(v)      (((v) & 0xFFFu) << 0)

/* ---- P1BPRCR / P1BPRSR: bad pixel removal ---- */
#define DCMIPP_P1BPRCR_STRENGTH(v)     (((v) & 7u) << 1)  // 0 tolerant .. 7 aggressive
#define DCMIPP_P1BPRCR_STRENGTH_MASK   (7u << 1)
#define DCMIPP_P1BPRCR_ENABLE          (1u << 0)          // raw Bayer only
#define DCMIPP_P1BPRSR_BADCNT_MASK     (0xFFFu << 0)

/* ---- P1DECR / PxDCCR: decimation (same layout) ---- */
#define DCMIPP_DEC_VDEC(v)             (((v) & 3u) << 3)  // 0:1 1:1/2 2:1/4 3:1/8
#define DCMIPP_DEC_HDEC(v)             (((v) & 3u) << 1)
#define DCMIPP_DEC_ENABLE              (1u << 0)
#define DCMIPP_P1DECR_VDEC(v)          DCMIPP_DEC_VDEC(v)
#define DCMIPP_P1DECR_HDEC(v)          DCMIPP_DEC_HDEC(v)
#define DCMIPP_P1DECR_ENABLE           DCMIPP_DEC_ENABLE
#define DCMIPP_PxDCCR_VDEC(v)          DCMIPP_DEC_VDEC(v)
#define DCMIPP_PxDCCR_HDEC(v)          DCMIPP_DEC_HDEC(v)
#define DCMIPP_PxDCCR_ENABLE           DCMIPP_DEC_ENABLE

/* ---- P1BLCCR: black level (8-bit, subtracted MSB-aligned: RAW10 -> BLC << 2) ---- */
#define DCMIPP_P1BLCCR_BLCR(v)         (((v) & 0xFFu) << 24)
#define DCMIPP_P1BLCCR_BLCG(v)         (((v) & 0xFFu) << 16)
#define DCMIPP_P1BLCCR_BLCB(v)         (((v) & 0xFFu) << 8)
#define DCMIPP_P1BLCCR_ENABLE          (1u << 0)

/* ---- P1EXCR1/2: exposure + WB gain = MULT/128 * 2^SHF ---- */
#define DCMIPP_P1EXCR1_SHFR(v)         (((v) & 7u) << 28)
#define DCMIPP_P1EXCR1_MULTR(v)        (((v) & 0xFFu) << 20)
#define DCMIPP_P1EXCR1_ENABLE          (1u << 0)          // enables all three channels
#define DCMIPP_P1EXCR2_SHFG(v)         (((v) & 7u) << 28)
#define DCMIPP_P1EXCR2_MULTG(v)        (((v) & 0xFFu) << 20)
#define DCMIPP_P1EXCR2_SHFB(v)         (((v) & 7u) << 12)
#define DCMIPP_P1EXCR2_MULTB(v)        (((v) & 0xFFu) << 4)

/* ---- P1STyCR: statistics y = 1..3 ---- */
#define DCMIPP_P1STCR_MODE_BINS        (1u << 7)          // 0 = average, 1 = bins
#define DCMIPP_P1STCR_SRC(v)           (((v) & 7u) << 4)
#define DCMIPP_P1STCR_SRC_MASK         (7u << 4)
#define DCMIPP_STSRC_R_PRE             0  // after decimation (before BLC/exposure); raw: only R sites
#define DCMIPP_STSRC_G_PRE             1
#define DCMIPP_STSRC_B_PRE             2
#define DCMIPP_STSRC_L_PRE             3  // L = R/4 + G/2 + B/4
#define DCMIPP_STSRC_R_POST            4  // after BLC + exposure + demosaicing
#define DCMIPP_STSRC_G_POST            5
#define DCMIPP_STSRC_B_POST            6
#define DCMIPP_STSRC_L_POST            7
#define DCMIPP_P1STCR_BINS(v)          (((v) & 3u) << 2)
#define DCMIPP_P1STCR_BINS_MASK        (3u << 2)
#define DCMIPP_P1STCR_ENABLE           (1u << 0)
#define DCMIPP_P1STSTR_VSTART(v)       (((v) & 0xFFFu) << 16)
#define DCMIPP_P1STSTR_HSTART(v)       (((v) & 0xFFFu) << 0)
#define DCMIPP_P1STSZR_CROPEN          (1u << 31)
#define DCMIPP_P1STSZR_VSIZE(v)        (((v) & 0xFFFu) << 16)
#define DCMIPP_P1STSZR_HSIZE(v)        (((v) & 0xFFFu) << 0)
#define DCMIPP_P1STSR_ACCU_MASK        (0xFFFFFFu << 0)   // 32-bit accumulator / 256
#define DCMIPP_P1CSTCR_ACCU(r)         (((r) >> 8) & 0xFFFFFFu) // ACCU copy in P1CSTyCR[31:8]

/* ---- P1DMCR: demosaicing ---- */
#define DCMIPP_P1DMCR_EDGE(v)          (((v) & 7u) << 28)
#define DCMIPP_P1DMCR_LINEH(v)         (((v) & 7u) << 24)
#define DCMIPP_P1DMCR_LINEV(v)         (((v) & 7u) << 20)
#define DCMIPP_P1DMCR_PEAK(v)          (((v) & 7u) << 16)
#define DCMIPP_P1DMCR_TYPE(v)          (((v) & 3u) << 1)  // 0 RGGB, 1 GRBG, 2 GBRG, 3 BGGR
#define DCMIPP_P1DMCR_TYPE_MASK        (3u << 1)
#define DCMIPP_P1DMCR_ENABLE           (1u << 0)
#define DCMIPP_BAYER_RGGB              0
#define DCMIPP_BAYER_GRBG              1
#define DCMIPP_BAYER_GBRG              2
#define DCMIPP_BAYER_BGGR              3

/* ---- P1CCCR / P1YUVCR (same layout) ---- */
#define DCMIPP_CC_CLAMP                (1u << 2)
#define DCMIPP_CC_TYPE_RGB             (1u << 1)  // clamp type: 0 = Y [16;235] UV [16;240], 1 = RGB [16;235]
#define DCMIPP_CC_ENABLE               (1u << 0)
/* Coefficient registers (P1CCxR1/2 and P1YUVxR1/2, x = R, G, B):
 *   xR1: bits 26:16 = col 2 (xG), bits 10:0 = col 1 (xR)   11-bit signed 2.8 (1.0 = 0x100)
 *   xR2: bits 25:16 = added (xA, 10-bit signed integer), bits 10:0 = col 3 (xB)
 * Row R = output R (Cr/V for YUV), row G = output G (Y), row B = output B (Cb/U). */
#define DCMIPP_CC_COEF(v)              ((uint32_t)(v) & 0x7FFu)  // signed 2.8 -> 11 bits
#define DCMIPP_CC_ADD(v)               ((uint32_t)(v) & 0x3FFu)  // signed int -> 10 bits
#define DCMIPP_CC_R1(col1, col2)       ((DCMIPP_CC_COEF(col2) << 16) | DCMIPP_CC_COEF(col1))
#define DCMIPP_CC_R2(col3, add)        ((DCMIPP_CC_ADD(add) << 16) | DCMIPP_CC_COEF(col3))

/* ---- P1CTCR1..3: contrast (LUMx 6 bits, 16 = x1.0) ---- */
#define DCMIPP_P1CTCR1_LUM0(v)         (((v) & 0x3Fu) << 9)
#define DCMIPP_P1CTCR1_ENABLE          (1u << 0)
#define DCMIPP_P1CTCR2_LUM1(v)         (((v) & 0x3Fu) << 25)
#define DCMIPP_P1CTCR2_LUM2(v)         (((v) & 0x3Fu) << 17)
#define DCMIPP_P1CTCR2_LUM3(v)         (((v) & 0x3Fu) << 9)
#define DCMIPP_P1CTCR2_LUM4(v)         (((v) & 0x3Fu) << 1)
#define DCMIPP_P1CTCR3_LUM5(v)         (((v) & 0x3Fu) << 25)
#define DCMIPP_P1CTCR3_LUM6(v)         (((v) & 0x3Fu) << 17)
#define DCMIPP_P1CTCR3_LUM7(v)         (((v) & 0x3Fu) << 9)
#define DCMIPP_P1CTCR3_LUM8(v)         (((v) & 0x3Fu) << 1)

/* ---- PxCRSTR / PxCRSZR: crop (pixels) ---- */
#define DCMIPP_PxCRSTR_VSTART(v)       (((v) & 0xFFFu) << 16)
#define DCMIPP_PxCRSTR_HSTART(v)       (((v) & 0xFFFu) << 0)
#define DCMIPP_PxCRSZR_ENABLE          (1u << 31)
#define DCMIPP_PxCRSZR_VSIZE(v)        (((v) & 0xFFFu) << 16)  // 0 -> forced to 0xFFE
#define DCMIPP_PxCRSZR_HSIZE(v)        (((v) & 0xFFFu) << 0)

/* ---- PxDSCR / PxDSRTIOR / PxDSSZR: downsize ---- */
#define DCMIPP_PxDSCR_ENABLE           (1u << 31)
#define DCMIPP_PxDSCR_VDIV(v)          (((v) & 0x3FFu) << 16)  // 128 (8x) .. 1023 (1x)
#define DCMIPP_PxDSCR_HDIV(v)          (((v) & 0x3FFu) << 0)
#define DCMIPP_PxDSRTIOR_VRATIO(v)     (((v) & 0xFFFFu) << 16) // 3.13: 8192 (1x) .. 65535 (8x)
#define DCMIPP_PxDSRTIOR_HRATIO(v)     (((v) & 0xFFFFu) << 0)
#define DCMIPP_PxDSSZR_VSIZE(v)        (((v) & 0xFFFu) << 16)  // output size; 0 disables that dimension
#define DCMIPP_PxDSSZR_HSIZE(v)        (((v) & 0xFFFu) << 0)

/* ---- PxCMRICR / PxRIyCR1/2: ROI ---- */
#define DCMIPP_PxCMRICR_ROIEN(y)       (1u << (15 + (y)))      // y = 1..8 -> bits 16..23
#define DCMIPP_PxCMRICR_ROILSZ(v)      (((v) & 3u) << 0)       // line width 1, 2, 4, 8 px
#define DCMIPP_RICR1_CLR(v)            (((v) & 3u) << 28)
#define DCMIPP_RICR1_VSTART(v)         (((v) & 0xFFFu) << 16)
#define DCMIPP_RICR1_CLG(v)            (((v) & 3u) << 14)
#define DCMIPP_RICR1_CLB(v)            (((v) & 3u) << 12)
#define DCMIPP_RICR1_HSTART(v)         (((v) & 0xFFFu) << 0)
#define DCMIPP_RICR2_VSIZE(v)          (((v) & 0xFFFu) << 16)
#define DCMIPP_RICR2_HSIZE(v)          (((v) & 0xFFFu) << 0)

/* ---- PxGMCR ---- */
#define DCMIPP_PxGMCR_ENABLE           (1u << 0)
