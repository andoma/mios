#pragma once

// STM32N6 CSI-2 Host + D-PHY RX register definitions
// Source: RM0486 Rev 2, chapter 40 (Table 371 register map), cross-checked
// against CMSIS stm32n657xx.h (CSI_TypeDef and bit defines) from STM32CubeN6.
//
// Memory map (RM0486 Table "memory map", APB5):
//   0x58006000 - 0x58007FFF  secure alias     "CSI2 HOST wrapper" (8 KB)
//   0x48006000 - 0x48007FFF  nonsecure alias
// The rest of the stm32n6 platform uses the secure aliases (RCC 0x56028000,
// SYSCFG 0x56008000, ...), and Mios runs secure, so use the secure alias.
// With RIFSC default (RISUP 92 SEC=0, PRIV=0) the peripheral is nonsecure;
// RIFSC only blocks *nonsecure* accesses to secure-configured peripherals,
// so secure accesses through 0x5800_6000 are fine either way.

#define CSI_BASE_S   0x58006000
#define CSI_BASE_NS  0x48006000
#define CSI_BASE     CSI_BASE_S


// NVIC
#define CSI_IRQ      47   // "CSI global interrupt", vector offset 0x0FC

// RIFSC: RISUP index 92 (CSI2H, covers 0x0000 host + 0x1000 PHY wrap)
//   -> RIFSC_RISC_SECCFGR2 / PRIVCFGR2 bit 28  (92 = 2*32 + 28)
// DCMIPP: RISUP 93 -> reg 2 bit 29; DCMIPP AXI master = RIMC index 9
#define RIF_PERIPH_CSI_REG     2
#define RIF_PERIPH_CSI_BIT     28

// =====================================================================
// CSI-2 Host registers
// =====================================================================

#define CSI_CR          (CSI_BASE + 0x000)  // control
#define CSI_PCR         (CSI_BASE + 0x004)  // DPHY_RX control
//                                 0x008-0x00C reserved
#define CSI_VCxCFGR1(x) (CSI_BASE + 0x010 + 0x10 * (x))  // x = 0..3
#define CSI_VCxCFGR2(x) (CSI_BASE + 0x014 + 0x10 * (x))
#define CSI_VCxCFGR3(x) (CSI_BASE + 0x018 + 0x10 * (x))
#define CSI_VCxCFGR4(x) (CSI_BASE + 0x01C + 0x10 * (x))
#define CSI_VC0CFGR1    (CSI_BASE + 0x010)
#define CSI_VC0CFGR2    (CSI_BASE + 0x014)
#define CSI_VC0CFGR3    (CSI_BASE + 0x018)
#define CSI_VC0CFGR4    (CSI_BASE + 0x01C)
#define CSI_VC1CFGR1    (CSI_BASE + 0x020)
#define CSI_VC1CFGR2    (CSI_BASE + 0x024)
#define CSI_VC1CFGR3    (CSI_BASE + 0x028)
#define CSI_VC1CFGR4    (CSI_BASE + 0x02C)
#define CSI_VC2CFGR1    (CSI_BASE + 0x030)
#define CSI_VC2CFGR2    (CSI_BASE + 0x034)
#define CSI_VC2CFGR3    (CSI_BASE + 0x038)
#define CSI_VC2CFGR4    (CSI_BASE + 0x03C)
#define CSI_VC3CFGR1    (CSI_BASE + 0x040)
#define CSI_VC3CFGR2    (CSI_BASE + 0x044)
#define CSI_VC3CFGR3    (CSI_BASE + 0x048)
#define CSI_VC3CFGR4    (CSI_BASE + 0x04C)
#define CSI_LBxCFGR(x)  (CSI_BASE + 0x050 + 0x04 * (x))  // x = 0..3
#define CSI_LB0CFGR     (CSI_BASE + 0x050)
#define CSI_LB1CFGR     (CSI_BASE + 0x054)
#define CSI_LB2CFGR     (CSI_BASE + 0x058)
#define CSI_LB3CFGR     (CSI_BASE + 0x05C)
#define CSI_TIMxCFGR(x) (CSI_BASE + 0x060 + 0x04 * (x))  // x = 0..3
#define CSI_TIM0CFGR    (CSI_BASE + 0x060)
#define CSI_TIM1CFGR    (CSI_BASE + 0x064)
#define CSI_TIM2CFGR    (CSI_BASE + 0x068)
#define CSI_TIM3CFGR    (CSI_BASE + 0x06C)
#define CSI_LMCFGR      (CSI_BASE + 0x070)  // lane merger, reset 0x43210200
#define CSI_PRGITR      (CSI_BASE + 0x074)  // programmable interrupts
#define CSI_WDR         (CSI_BASE + 0x078)  // watchdog
//                                 0x07C reserved
#define CSI_IER0        (CSI_BASE + 0x080)
#define CSI_IER1        (CSI_BASE + 0x084)
//                                 0x088-0x08C reserved
#define CSI_SR0         (CSI_BASE + 0x090)
#define CSI_SR1         (CSI_BASE + 0x094)
//                                 0x098-0x0FC reserved
#define CSI_FCR0        (CSI_BASE + 0x100)  // write-1-to-clear SR0
#define CSI_FCR1        (CSI_BASE + 0x104)  // write-1-to-clear SR1
//                                 0x108-0x10C reserved
#define CSI_SPDFR       (CSI_BASE + 0x110)  // short packet data field (ro)
#define CSI_ERR1        (CSI_BASE + 0x114)  // ro
#define CSI_ERR2        (CSI_BASE + 0x118)  // ro

// =====================================================================
// CSI PHY (D-PHY RX wrapper) registers
// =====================================================================

#define CSI_PRCR        (CSI_BASE + 0x1000) // PHY reset control,     reset 0x00000000
#define CSI_PMCR        (CSI_BASE + 0x1004) // PHY mode control,      reset 0x00011003
#define CSI_PFCR        (CSI_BASE + 0x1008) // PHY frequency control, reset 0x00000017
//                                 0x100C reserved
#define CSI_PTCR0       (CSI_BASE + 0x1010) // PHY test control 0,    reset 0x00000001
#define CSI_PTCR1       (CSI_BASE + 0x1014) // PHY test control 1,    reset 0x00000000
#define CSI_PTSR        (CSI_BASE + 0x1018) // PHY test status (ro)

// =====================================================================
// Bit fields
// =====================================================================

// CSI_CR (0x00). VCxSTART/VCxSTOP are write-only pulses, read as 0.
// Never write START and STOP of the same VC in one write.
#define CSI_CR_CSIEN            (1u << 0)
#define CSI_CR_VCxSTART(x)      (1u << (2 + 4 * (x)))   // 2, 6, 10, 14
#define CSI_CR_VCxSTOP(x)       (1u << (3 + 4 * (x)))   // 3, 7, 11, 15
#define CSI_CR_VC0START         (1u << 2)
#define CSI_CR_VC0STOP          (1u << 3)
#define CSI_CR_VC1START         (1u << 6)
#define CSI_CR_VC1STOP          (1u << 7)
#define CSI_CR_VC2START         (1u << 10)
#define CSI_CR_VC2STOP          (1u << 11)
#define CSI_CR_VC3START         (1u << 14)
#define CSI_CR_VC3STOP          (1u << 15)

// CSI_PCR (0x04)
// RM: PWRDOWN 1 = power down, 0 = active.  BUT the ST HAL writes
// DL0EN|DL1EN|CLEN|PWRDOWN to *enable* the PHY (Synopsys shutdownz is
// active-low).  UNSURE which is right: the HAL is proven on STM32N6570-DK
// with IMX335, so follow the HAL (set PWRDOWN=1 together with lane enables).
#define CSI_PCR_PWRDOWN         (1u << 0)
#define CSI_PCR_CLEN            (1u << 1)
#define CSI_PCR_DL0EN           (1u << 2)
#define CSI_PCR_DL1EN           (1u << 3)

// CSI_VCxCFGR1 (0x10 + 0x10*x). Writes while VCx is running are ignored.
#define CSI_VCCFGR1_ALLDT       (1u << 0)
#define CSI_VCCFGR1_DTyEN(y)    (1u << (1 + (y)))       // y = 0..6, bits 1..7
#define CSI_VCCFGR1_CDTFT_SHIFT 8                       // [12:8] common format (ALLDT=1)
#define CSI_VCCFGR1_CDTFT_MASK  (0x1fu << 8)
#define CSI_VCCFGR1_DT0_SHIFT   16                      // [21:16] DT0 data type
#define CSI_VCCFGR1_DT0_MASK    (0x3fu << 16)
#define CSI_VCCFGR1_DT0FT_SHIFT 24                      // [28:24] DT0 format
#define CSI_VCCFGR1_DT0FT_MASK  (0x1fu << 24)

// CSI_VCxCFGR2/3/4: low half = DT(2n-1), high half = DT(2n)
//   CFGR2: DT1 [5:0], DT1FT [12:8], DT2 [21:16], DT2FT [28:24]
//   CFGR3: DT3 [5:0], DT3FT [12:8], DT4 [21:16], DT4FT [28:24]
//   CFGR4: DT5 [5:0], DT5FT [12:8], DT6 [21:16], DT6FT [28:24]
#define CSI_VCCFGRn_DTLO_SHIFT   0
#define CSI_VCCFGRn_DTLOFT_SHIFT 8
#define CSI_VCCFGRn_DTHI_SHIFT   16
#define CSI_VCCFGRn_DTHIFT_SHIFT 24

// Format codes for CDTFT / DTyFT
#define CSI_FT_BPP6             0x0
#define CSI_FT_BPP7             0x1
#define CSI_FT_BPP8             0x2   // RAW8, YUV8, RGB444/555/565/888, JPEG
#define CSI_FT_BPP10            0x3   // RAW10, YUV10
#define CSI_FT_BPP12            0x4
#define CSI_FT_BPP14            0x5
#define CSI_FT_BPP16            0x6

// MIPI CSI-2 data type codes (used for DTy and in DCMIPP flow selection)
#define CSI_DT_FS               0x00
#define CSI_DT_FE               0x01
#define CSI_DT_LS               0x02
#define CSI_DT_LE               0x03
#define CSI_DT_EMBEDDED         0x12
#define CSI_DT_YUV422_8         0x1E
#define CSI_DT_RGB565           0x22
#define CSI_DT_RGB888           0x24
#define CSI_DT_RAW8             0x2A
#define CSI_DT_RAW10            0x2B
#define CSI_DT_RAW12            0x2C

// CSI_LBxCFGR (0x50 + 4*x). Ignored while LBxEN=1.
#define CSI_LBCFGR_BYTECNT_SHIFT 0    // [15:0]
#define CSI_LBCFGR_LINECNT_SHIFT 16   // [31:16]

// CSI_TIMxCFGR (0x60 + 4*x)
#define CSI_TIMCFGR_COUNT_MASK  0x01ffffffu  // [24:0], clk_proc cycles

// CSI_LMCFGR (0x70). Only write with CSIEN=0 and the PHY not clocked.
// Reset value 0x43210200 (= LANENB 2, DL0MAP 1, DL1MAP 2, plus reserved
// bits that look like unused DL2/DL3MAP of a 4-lane IP; writing 0 there is
// what the HAL does via WRITE_REG).
#define CSI_LMCFGR_LANENB_SHIFT 8     // [10:8] 1 = 1 lane, 2 = 2 lanes
#define CSI_LMCFGR_LANENB_MASK  (7u << 8)
#define CSI_LMCFGR_DL0MAP_SHIFT 16    // [18:16] physical DL0 -> logical (1 = L0, 2 = L1)
#define CSI_LMCFGR_DL0MAP_MASK  (7u << 16)
#define CSI_LMCFGR_DL1MAP_SHIFT 20    // [22:20] physical DL1 -> logical (1 = L0, 2 = L1)
#define CSI_LMCFGR_DL1MAP_MASK  (7u << 20)
// Straight mapping, 2 lanes:
#define CSI_LMCFGR_2LANE_STRAIGHT \
  ((2u << CSI_LMCFGR_LANENB_SHIFT) | (1u << CSI_LMCFGR_DL0MAP_SHIFT) | \
   (2u << CSI_LMCFGR_DL1MAP_SHIFT))                       // 0x00210200

// CSI_PRGITR (0x74)
//   LBx: LBxVC [1:0] at 4*x, LBxEN at 4*x+3.  LBxEN only with CSIEN=1.
//   TIMx: TIMxVC [1:0] at 16+4*x, TIMxEOF at 18+4*x, TIMxEN at 19+4*x.
//         TIM fields only written with CSIEN=0.
#define CSI_PRGITR_LBVC_SHIFT(x)   (4 * (x))
#define CSI_PRGITR_LBEN(x)         (1u << (4 * (x) + 3))
#define CSI_PRGITR_TIMVC_SHIFT(x)  (16 + 4 * (x))
#define CSI_PRGITR_TIMEOF(x)       (1u << (16 + 4 * (x) + 2))
#define CSI_PRGITR_TIMEN(x)        (1u << (16 + 4 * (x) + 3))

// CSI_WDR (0x78): CNT[31:0], clk_proc cycles between EOF and SOF, 0 = off

// CSI_IER0 (0x80) / CSI_SR0 (0x90) / CSI_FCR0 (0x100) share bit positions
// (IE suffix / F suffix / C prefix respectively), except VCxSTATEF which
// exist only in SR0.
#define CSI_SR0_LBxF(x)         (1u << (x))         // bits 0..3
#define CSI_SR0_TIMxF(x)        (1u << (4 + (x)))   // bits 4..7
#define CSI_SR0_SOFxF(x)        (1u << (8 + (x)))   // bits 8..11
#define CSI_SR0_EOFxF(x)        (1u << (12 + (x)))  // bits 12..15
#define CSI_SR0_SOF0F           (1u << 8)
#define CSI_SR0_EOF0F           (1u << 12)
#define CSI_SR0_SPKTF           (1u << 16)  // generic short packet -> CSI_SPDFR
#define CSI_SR0_VCxSTATEF(x)    (1u << (17 + (x)))  // bits 17..20, SR0 only
#define CSI_SR0_VC0STATEF       (1u << 17)
#define CSI_SR0_CCFIFOFF        (1u << 21)  // clock changer FIFO full (IER0: CCFIFOFIE)
#define CSI_SR0_CRCERRF         (1u << 24)
#define CSI_SR0_ECCERRF         (1u << 25)  // multi-bit header ECC
#define CSI_SR0_CECCERRF        (1u << 26)  // corrected 1-bit ECC (info)
#define CSI_SR0_IDERRF          (1u << 27)  // reserved data type (info)
#define CSI_SR0_SPKTERRF        (1u << 28)  // shorter-than-expected packet
#define CSI_SR0_WDERRF          (1u << 29)
#define CSI_SR0_SYNCERRF        (1u << 30)  // SOF in frame / EOF in blanking
#define CSI_SR0_ERRMASK         (0x7f000000u | CSI_SR0_CCFIFOFF)

#define CSI_IER0_LBxIE(x)       CSI_SR0_LBxF(x)
#define CSI_IER0_TIMxIE(x)      CSI_SR0_TIMxF(x)
#define CSI_IER0_SOFxIE(x)      CSI_SR0_SOFxF(x)
#define CSI_IER0_EOFxIE(x)      CSI_SR0_EOFxF(x)
#define CSI_IER0_SPKTIE         CSI_SR0_SPKTF
#define CSI_IER0_CCFIFOFIE      CSI_SR0_CCFIFOFF
#define CSI_IER0_CRCERRIE       CSI_SR0_CRCERRF
#define CSI_IER0_ECCERRIE       CSI_SR0_ECCERRF
#define CSI_IER0_CECCERRIE      CSI_SR0_CECCERRF
#define CSI_IER0_IDERRIE        CSI_SR0_IDERRF
#define CSI_IER0_SPKTERRIE      CSI_SR0_SPKTERRF
#define CSI_IER0_WDERRIE        CSI_SR0_WDERRF
#define CSI_IER0_SYNCERRIE      CSI_SR0_SYNCERRF
// FCR0 clear-all value (all implemented clear bits): 0x7F21FFFF
#define CSI_FCR0_ALL            0x7f21ffffu

// CSI_IER1 (0x84) / CSI_SR1 (0x94) low half / CSI_FCR1 (0x104):
// D-PHY lane error flags. Lane n base bit = 8*n.
#define CSI_SR1_ESOTDLxF(n)     (1u << (8 * (n) + 0))  // SOT error (sync still OK)
#define CSI_SR1_ESOTSYNCDLxF(n) (1u << (8 * (n) + 1))  // SOT sync error
#define CSI_SR1_EESCDLxF(n)     (1u << (8 * (n) + 2))  // escape entry error
#define CSI_SR1_ESYNCESCDLxF(n) (1u << (8 * (n) + 3))  // LP data sync error
#define CSI_SR1_ECTRLDLxF(n)    (1u << (8 * (n) + 4))  // control error
#define CSI_SR1_ERRMASK         0x00001f1fu
#define CSI_IER1_ALL            0x00001f1fu
#define CSI_FCR1_ALL            0x00001f1fu

// CSI_SR1 high half: live D-PHY PPI status (read-only, not latched)
#define CSI_SR1_ACTDL0F         (1u << 16)  // RxActiveHS_lane0
#define CSI_SR1_SYNCDL0F        (1u << 17)  // RxSyncHS_lane0
#define CSI_SR1_SKCALDL0F       (1u << 18)  // RxSkewCalHS_lane0
#define CSI_SR1_STOPDL0F        (1u << 19)  // StopState_D_lane0
#define CSI_SR1_ULPNDL0F        (1u << 20)  // UlpsActiveNot_D_lane0
#define CSI_SR1_ACTDL1F         (1u << 22)
#define CSI_SR1_SYNCDL1F        (1u << 23)
#define CSI_SR1_SKCALDL1F       (1u << 24)
#define CSI_SR1_STOPDL1F        (1u << 25)
#define CSI_SR1_ULPNDL1F        (1u << 26)
#define CSI_SR1_STOPCLF         (1u << 28)  // StopStateC (clock lane)
#define CSI_SR1_ULPNACTF        (1u << 29)  // UlpsactiveNotC
#define CSI_SR1_ULPNCLF         (1u << 30)  // RxUlpsClkNot
#define CSI_SR1_ACTCLF          (1u << 31)  // RxClkactiveHs

// CSI_SPDFR (0x110), read-only, last generic short packet
#define CSI_SPDFR_DATAFIELD_MASK 0xffffu        // [15:0]
#define CSI_SPDFR_DATATYPE_SHIFT 16             // [21:16]
#define CSI_SPDFR_VCHANNEL_SHIFT 22             // [23:22]

// CSI_ERR1 (0x114), read-only
#define CSI_ERR1_CRCDTERR_SHIFT  0    // [5:0]
#define CSI_ERR1_CRCVCERR_SHIFT  6    // [7:6]
#define CSI_ERR1_CECCDTERR_SHIFT 8    // [13:8]
#define CSI_ERR1_CECCVCERR_SHIFT 14   // [15:14]
#define CSI_ERR1_IDDTERR_SHIFT   16   // [21:16]
#define CSI_ERR1_IDVCERR_SHIFT   22   // [23:22]

// CSI_ERR2 (0x118), read-only
#define CSI_ERR2_SPKTDTERR_SHIFT 0    // [5:0]
#define CSI_ERR2_SPKTVCERR_SHIFT 6    // [7:6]
#define CSI_ERR2_WDVCERR_SHIFT   16   // [17:16]
#define CSI_ERR2_SYNCVCERR_SHIFT 18   // [19:18]

// CSI_PRCR (0x1000)
#define CSI_PRCR_PEN            (1u << 1)   // 0 = D-PHY digital in reset, 1 = out of reset

// CSI_PMCR (0x1004), reset 0x00011003 (FRXMDL0|FRXMDL1|TUESDL0|TUEXDL0).
// HAL clears the whole register after PEN=1 ("remove the force").
#define CSI_PMCR_FRXMDL0        (1u << 0)   // force DL0 into RX / wait for stop state
#define CSI_PMCR_FRXMDL1        (1u << 1)   // force DL1 into RX / wait for stop state
#define CSI_PMCR_FTXSMDL0       (1u << 2)
#define CSI_PMCR_DTDL           (1u << 4)   // disable turn-around DL0
#define CSI_PMCR_RTDL0          (1u << 8)
#define CSI_PMCR_TUESDL0        (1u << 12)
#define CSI_PMCR_TUEXDL0        (1u << 16)

// CSI_PFCR (0x1008), reset 0x17
#define CSI_PFCR_CCFR_SHIFT     0           // [5:0] cfgclkfreqrange = round((Fcfg_MHz - 17) * 4)
#define CSI_PFCR_CCFR_MASK      (0x3fu << 0)
#define CSI_PFCR_HSFR_SHIFT     8           // [14:8] hsfreqrange, set while PHY powered down
#define CSI_PFCR_HSFR_MASK      (0x7fu << 8)
// RM: DLD 0 = Rx, 1 = Tx.  HAL sets DLD=1 with comment
// "basedir_0 ... Synopsys 1 RX 0 TX".  UNSURE; follow the HAL (DLD=1).
#define CSI_PFCR_DLD            (1u << 16)

// CSI_PTCR0 (0x1010), reset 0x1
#define CSI_PTCR0_TCKEN         (1u << 0)   // testclk
#define CSI_PTCR0_TRSEN         (1u << 1)   // testclr

// CSI_PTCR1 (0x1014)
#define CSI_PTCR1_TDI_MASK      0xffu       // [7:0] testdin
#define CSI_PTCR1_TWM           (1u << 16)  // testen (1 = address/testcode phase)

// CSI_PTSR (0x1018)
#define CSI_PTSR_TDO_MASK       0xffu       // [7:0] testdout

// =====================================================================
// D-PHY test-code registers (Synopsys DWC MIPI D-PHY RX), as used by the
// ST HAL (HAL_DCMIPP_CSI_SetConfig). Not documented in RM0486.
// =====================================================================
#define DPHY_TC_DESKEW_POL      0x08  // HAL writes 0x38 ("deskew_polarity_rw 1'b1")
#define DPHY_TC_DES_EN_CNT      0xE4  // HAL writes 0x11 ("counter_for_des_en_config_if_rx 0x10
                                      //  + DLL prog EN", valid for 13 <= CCFR <= 38)
#define DPHY_TC_OSC_FREQ_LO     0xE2  // osc_freq_target[7:0]   UNSURE: HAL writes 0xE3 twice
#define DPHY_TC_OSC_FREQ_HI     0xE3  // osc_freq_target[11:8]  (see csi_notes.md)

// =====================================================================
// hsfreqrange / osc_freq_target table, verbatim from ST HAL
// (stm32n6xx_hal_dcmipp.c, SNPS_Freqs[63]); first column is the HAL
// DCMIPP_CSI_PHY_BT_xxx nominal lane rate in Mbit/s (index = enum value).
// X(mbps, hsfreqrange, osc_freq_target)
// No such table exists in RM0486.
// =====================================================================
#define CSI_DPHY_FREQ_TABLE(X) \
  X(  80, 0x00, 460) \
  X(  90, 0x10, 460) \
  X( 100, 0x20, 460) \
  X( 110, 0x30, 460) \
  X( 120, 0x01, 460) \
  X( 130, 0x11, 460) \
  X( 140, 0x21, 460) \
  X( 150, 0x31, 460) \
  X( 160, 0x02, 460) \
  X( 170, 0x12, 460) \
  X( 180, 0x22, 460) \
  X( 190, 0x32, 460) \
  X( 205, 0x03, 460) \
  X( 220, 0x13, 460) \
  X( 235, 0x23, 460) \
  X( 250, 0x33, 460) \
  X( 275, 0x04, 460) \
  X( 300, 0x14, 460) \
  X( 325, 0x25, 460) \
  X( 350, 0x35, 460) \
  X( 400, 0x05, 460) \
  X( 450, 0x16, 460) \
  X( 500, 0x26, 460) \
  X( 550, 0x37, 460) \
  X( 600, 0x07, 460) \
  X( 650, 0x18, 460) \
  X( 700, 0x28, 460) \
  X( 750, 0x39, 460) \
  X( 800, 0x09, 460) \
  X( 850, 0x19, 460) \
  X( 900, 0x29, 460) \
  X( 950, 0x3A, 460) \
  X(1000, 0x0A, 460) \
  X(1050, 0x1A, 460) \
  X(1100, 0x2A, 460) \
  X(1150, 0x3B, 460) \
  X(1200, 0x0B, 460) \
  X(1250, 0x1B, 460) \
  X(1300, 0x2B, 460) \
  X(1350, 0x3C, 460) \
  X(1400, 0x0C, 460) \
  X(1450, 0x1C, 460) \
  X(1500, 0x2C, 460) \
  X(1550, 0x3D, 285) \
  X(1600, 0x0D, 295) \
  X(1650, 0x1D, 304) \
  X(1700, 0x2E, 313) \
  X(1750, 0x3E, 322) \
  X(1800, 0x0E, 331) \
  X(1850, 0x1E, 341) \
  X(1900, 0x2F, 350) \
  X(1950, 0x3F, 359) \
  X(2000, 0x0F, 368) \
  X(2050, 0x40, 377) \
  X(2100, 0x41, 387) \
  X(2150, 0x42, 396) \
  X(2200, 0x43, 405) \
  X(2250, 0x44, 414) \
  X(2300, 0x45, 423) \
  X(2350, 0x46, 432) \
  X(2400, 0x47, 442) \
  X(2450, 0x48, 451) \
  X(2500, 0x49, 460)

// 800 Mbit/s/lane (our IMX335 target): HSFR = 0x09, osc_freq_target = 460
#define CSI_HSFR_800MBPS        0x09
#define CSI_OSCFREQ_800MBPS     460
