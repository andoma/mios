/*
 * Tegra234 (Orin) MGBE 10G Ethernet MAC driver for Mios.
 *
 * Targets the AGX devkit onboard 10G port: MGBE0, XFI 10G, Marvell/Aquantia
 * AQR113C PHY over Clause-45 MDIO. (The NVIDIA devkit DT labels this PHY
 * "88Q4364", but the chip on the board reports the Aquantia ID 0x31c3.1c13.)
 * The MGBE is a Synopsys DWC_xgmac core, so the MAC / MTL / DMA layout mirrors
 * the DWC GMAC driven by src/platform/stm32h7/stm32h7_eth.c; the descriptor
 * model and aarch64 cache handling follow src/drivers/rtl8168.c.
 *
 * Register sequences are transcribed from NVIDIA's OS-agnostic OSI layer
 * (nvethernetrm, L4T r36.4): osi/core/mgbe_core.c, osi/dma/osi_dma*.c,
 * osi/core/xpcs.c.
 *
 * Scope: single DMA channel / single MTL queue, raw TX/RX for network boot.
 * No PTP, MACsec, TSO, RSS or multi-queue.
 *
 * Validated on the Jetson AGX Orin devkit (DHCP + HTTP netboot). The few
 * remaining per-board unknowns (production-board PHY interface mode, source
 * of the MAC address) are marked TODO(hw) inline.
 */

#include <assert.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <malloc.h>

#include <mios/mios.h>
#include <mios/driver.h>
#include <mios/eventlog.h>
#include <mios/task.h>
#include <mios/type_macros.h>

#include <net/pbuf.h>
#include <net/ether.h>

#include <mios/ethphy.h>

#include "reg.h"
#include "irq.h"
#include "cache.h"

#include "t234ccplex_clk.h"

#define MAC_NAME "t234-mgbe"

/* ------------------------------------------------------------------ */
/* MMIO bases (MGBE0). Other instances are at +0x100000 steps.        */
/* ------------------------------------------------------------------ */

#define MGBE0_MAC_BASE   0x06810000UL  /* reg-name "mac"  */
#define MGBE0_XPCS_BASE  0x068a0000UL  /* reg-name "xpcs" */
/*
 * DMA channel completion is routed to the per-VM IRQs (vm0..vm4), NOT the
 * "common" IRQ (which carries MAC-level events). The devkit vm-irq-config
 * maps DMA channels 0,1 -> vm0. interrupts[]: common=SPI384, vm0=SPI385.
 * Mios numbers SPIs as +32, so channel-0 completion = vm0 = 385 + 32.
 */
#define MGBE0_IRQ        (385 + 32)   /* vm0 */

/* ------------------------------------------------------------------ */
/* BPMP clock / reset IDs (dt-bindings/{clock,reset}/tegra234-*.h)     */
/* ------------------------------------------------------------------ */

#define CLK_MGBE0_RX_INPUT     248
#define CLK_MGBE0_RX_INPUT_M   357
#define CLK_MGBE0_RX_PCS_M     361
#define CLK_MGBE0_RX_PCS_INPUT 369
#define CLK_MGBE0_RX_PCS       373
#define CLK_MGBE0_TX           374
#define CLK_MGBE0_TX_PCS       375
#define CLK_MGBE0_MAC_DIVIDER  376
#define CLK_MGBE0_MAC          377
#define CLK_MGBE0_EEE_PCS      379
#define CLK_MGBE0_APP          380
#define CLK_MGBE0_PTP_REF      381

#define RESET_MGBE0_PCS        45
#define RESET_MGBE0_MAC        46

/* ------------------------------------------------------------------ */
/* Tegra234 main GPIO controller (nvidia,tegra234-gpio), "gpio"        */
/* (control) aperture. Per-pin register file:                         */
/*   base + bank*0x1000 + port*0x200 + pin*0x20                        */
/* ------------------------------------------------------------------ */

#define T234_GPIO_BASE 0x02210000UL
#define T234_GPIO_PIN(bank, port, pin) \
  (T234_GPIO_BASE + (bank) * 0x1000UL + (port) * 0x200UL + (pin) * 0x20UL)

#define T234_GPIO_ENABLE_CONFIG          0x00
#define T234_GPIO_ENABLE_CONFIG_ENABLE   (1u << 0)
#define T234_GPIO_ENABLE_CONFIG_OUT      (1u << 1)
#define T234_GPIO_OUTPUT_CONTROL         0x0c
#define T234_GPIO_OUTPUT_CONTROL_FLOATED (1u << 0)
#define T234_GPIO_OUTPUT_VALUE           0x10
#define T234_GPIO_OUTPUT_VALUE_HIGH      (1u << 0)

/* MGBE0 AQR113C PHY reset: devkit DT nvidia,phy-reset-gpio = Port Y pin 1
 * (TEGRA234_MAIN_GPIO(Y,1) = 145; port Y is bank 1, port 1). */
#define MGBE0_PHY_RESET_GPIO   T234_GPIO_PIN(1, 1, 1)
#define MGBE0_PHY_RST_DUR_US   221000  /* nvidia,phy-rst-duration-usec */
#define MGBE0_PHY_RST_PDELAY_US 150000 /* nvidia,phy-rst-pdelay-msec  */

/* ------------------------------------------------------------------ */
/* MAC region register offsets (from MAC base)                        */
/* ------------------------------------------------------------------ */

#define MGBE_MAC_TMCR        0x0000  /* Tx config; bit0 TE */
#define MGBE_MAC_RMCR        0x0004  /* Rx config; bit0 RE */
#define MGBE_MAC_PFR         0x0008  /* packet filter */
#define MGBE_MAC_RQC0R       0x00A0  /* RxQ enable control */
#define MGBE_MAC_RQC1R       0x00A4
#define MGBE_MAC_RX_FLW_CTRL 0x0090
#define MGBE_MAC_MDIO_SCCA   0x0200  /* MDIO single command address */
#define MGBE_MAC_MDIO_SCCD   0x0204  /* MDIO single command data */
#define MGBE_MAC_ADDRH(x)    (0x0300 + 0x8 * (x))
#define MGBE_MAC_ADDRL(x)    (0x0304 + 0x8 * (x))

#define MGBE_MAC_TMCR_TE     (1u << 0)
#define MGBE_MAC_TMCR_DDIC   (1u << 1)
#define MGBE_MAC_RMCR_RE     (1u << 0)
#define MGBE_MAC_RMCR_ACS    (1u << 1)
#define MGBE_MAC_RMCR_CST    (1u << 2)
#define MGBE_MAC_RMCR_IPC    (1u << 9)
#define MGBE_MAC_PFR_RA      (1u << 31) /* receive all */
#define MGBE_MAC_RQC1R_MCBCQEN    (1u << 15)
#define MGBE_MAC_RQC1R_MCBCQ_SHIFT 8
#define MGBE_MAC_ADDRH_AE    (1u << 31)
#define MGBE_MAC_RXQC0_RXQEN(q, v) (((v) & 0x3u) << ((q) * 2u))

#define MGBE_MDIO_SCCD_SBUSY (1u << 22)
#define MGBE_MDIO_SCCD_CMD_WR 1u
#define MGBE_MDIO_SCCD_CMD_RD 3u
#define MGBE_MDIO_SCCD_CMD_SHIFT 16
#define MGBE_MDIO_SCCD_CR_SHIFT  19   /* clock-range; OSI programs 0x5 */
#define MGBE_MDIO_SCCA_DA_SHIFT  21   /* Clause-45 device address */
#define MGBE_MDIO_SCCA_PA_SHIFT  16   /* PHY (port) address */

/* MTL */
#define MGBE_MTL_TX_OP_MODE(x) (0x1100 + 0x80 * (x))
#define MGBE_MTL_RX_OP_MODE(x) (0x1140 + 0x80 * (x))
#define MGBE_MTL_RXQ_DMA_MAP0  0x1030

#define MGBE_MTL_TSF           (1u << 1)
#define MGBE_MTL_TXQEN         (1u << 3)
#define MGBE_MTL_RSF           (1u << 5)
#define MGBE_MTL_TXQ_SIZE_SHIFT 16
#define MGBE_MTL_RXQ_SIZE_SHIFT 16
/* FIFO size code = (size_KB / 256) - 1. Q0 gets 160KB Rx, full Tx. */
#define MGBE_FIFO_SZ(kb)       ((((kb) * 1024u) / 256u) - 1u)
#define MGBE_RXQ0_FIFO_SZ      MGBE_FIFO_SZ(160)
#define MGBE_TXQ0_FIFO_SZ      MGBE_FIFO_SZ(128)
#define MGBE_RXQ_TO_DMA_MAP0   0x03020100u  /* Q0->ch0, Q1->ch1, ... */

/* DMA (channel x base = 0x80*x) */
#define MGBE_DMA_MODE          0x3000
#define MGBE_DMA_SBUS          0x3004
#define MGBE_DMA_MODE_SWR      (1u << 0)
#define MGBE_DMA_SBUS_UNDEF    (1u << 0)
#define MGBE_DMA_SBUS_BLEN256  (1u << 7)
#define MGBE_DMA_SBUS_EAME     (1u << 11)

#define MGBE_DMA_CHX_CTRL(x)      (0x3100 + 0x80 * (x))
#define MGBE_DMA_CHX_TX_CTRL(x)   (0x3104 + 0x80 * (x))
#define MGBE_DMA_CHX_RX_CTRL(x)   (0x3108 + 0x80 * (x))
#define MGBE_DMA_CHX_TDLH(x)      (0x3110 + 0x80 * (x))
#define MGBE_DMA_CHX_TDLA(x)      (0x3114 + 0x80 * (x))
#define MGBE_DMA_CHX_RDLH(x)      (0x3118 + 0x80 * (x))
#define MGBE_DMA_CHX_RDLA(x)      (0x311C + 0x80 * (x))
#define MGBE_DMA_CHX_TDTLP(x)     (0x3124 + 0x80 * (x)) /* Tx tail ptr   */
#define MGBE_DMA_CHX_RDTLP(x)     (0x312C + 0x80 * (x)) /* Rx tail ptr   */
#define MGBE_DMA_CHX_TX_CNTRL2(x) (0x3130 + 0x80 * (x)) /* Tx ring len   */
#define MGBE_DMA_CHX_RX_CNTRL2(x) (0x3134 + 0x80 * (x)) /* Rx ring len   */
#define MGBE_DMA_CHX_INTR_ENA(x)  (0x3138 + 0x80 * (x))
#define MGBE_DMA_CHX_STATUS(x)    (0x3160 + 0x80 * (x))

#define MGBE_DMA_CHX_CTRL_PBLX8   (1u << 16)
#define MGBE_DMA_CHX_TX_CTRL_ST   (1u << 0)
#define MGBE_DMA_CHX_TX_CTRL_OSP  (1u << 4)
#define MGBE_DMA_CHX_RX_CTRL_SR   (1u << 0)
#define MGBE_DMA_CHX_RX_CTRL_RBSZ_SHIFT 1   /* RBSZ field [14:1] */
#define MGBE_DMA_RING_LEN_MASK    0x3FFFu
#define MGBE_DMA_CHX_INTR_TIE     (1u << 0)
#define MGBE_DMA_CHX_INTR_RIE     (1u << 6)
#define MGBE_DMA_CHX_INTR_NIE     (1u << 15)
#define MGBE_DMA_CHX_STATUS_TI    (1u << 0)
#define MGBE_DMA_CHX_STATUS_RI    (1u << 6)
#define MGBE_DMA_CHX_STATUS_NIS   (1u << 15)

/* Tegra interrupt wrapper: muxes per-channel DMA IRQs onto the single
 * "common" GIC line. Without enabling CNTRL the common IRQ never fires. */
#define MGBE_VIRT_INTR_CHX_CNTRL(x)  (0x8600 + 0x8 * (x))
#define MGBE_VIRT_INTR_CHX_STATUS(x) (0x8604 + 0x8 * (x))
/* APB-space per-channel interrupt routing: selects which VM (GIC line) a
 * channel's interrupt is delivered to. Write BIT(vm_num). Channel 0 -> vm0. */
#define MGBE_VIRT_INTR_APB_CHX_CNTRL(x) (0x8200 + 0x4 * (x))
#define MGBE_VIRT_INTR_TX            (1u << 0) /* OSI_DMA_CH_TX_INTR */
#define MGBE_VIRT_INTR_RX            (1u << 1) /* OSI_DMA_CH_RX_INTR */

/* ------------------------------------------------------------------ */
/* XPCS register offsets (xpcs.c / xpcs.h)                            */
/* ------------------------------------------------------------------ */

/* WRAP regs: direct MMIO at xpcs_base + offset */
#define XPCS_ADDRESS                  0x03FC
#define XPCS_WRAP_UPHY_RX_CONTROL_0_0 0x801C
#define XPCS_WRAP_UPHY_HW_INIT_CTRL   0x8020
#define XPCS_WRAP_UPHY_STATUS         0x8044
#define XPCS_WRAP_IRQ_STATUS          0x8050

#define XPCS_WRAP_UPHY_HW_INIT_CTRL_TX_EN  (1u << 0)
#define XPCS_WRAP_UPHY_STATUS_TX_P_UP      (1u << 0)
#define XPCS_WRAP_IRQ_STATUS_PCS_LINK_STS  (1u << 6)
#define XPCS_RX_DATA_EN     (1u << 0)
#define XPCS_RX_IDDQ        (1u << 4)
#define XPCS_AUX_RX_IDDQ    (1u << 5)
#define XPCS_RX_SLEEP       ((1u << 6) | (1u << 7))
#define XPCS_RX_CAL_EN      (1u << 8)
#define XPCS_RX_CDR_RESET   (1u << 9)
#define XPCS_RX_PCS_PHY_RDY (1u << 10)
#define XPCS_RX_SW_OVRD     (1u << 31)

/* PCS-core regs: indirect-paged access (see xpcs_rd/xpcs_wr) */
#define XPCS_SR_XS_PCS_STS1      0xC0004
#define XPCS_SR_XS_PCS_CTRL2     0xC001C
#define XPCS_VR_XS_PCS_DIG_CTRL1 0xE0000
#define XPCS_SR_XS_PCS_STS1_RLU             (1u << 2)
#define XPCS_SR_XS_PCS_CTRL2_TYPE_BASE_R    0x0u
#define XPCS_VR_XS_PCS_DIG_CTRL1_USXG_EN    (1u << 9)
#define XPCS_VR_XS_PCS_DIG_CTRL1_VR_RST     (1u << 15)

#define XPCS_REG_ADDR_SHIFT 10u
#define XPCS_REG_ADDR_MASK  0x1FFFu
#define XPCS_REG_VALUE_MASK 0x3FFu

/* ------------------------------------------------------------------ */
/* Driver state                                                       */
/* ------------------------------------------------------------------ */

typedef struct {
  uint32_t w0;   /* buffer address low  */
  uint32_t w1;   /* buffer address high */
  uint32_t w2;
  uint32_t w3;
} desc_t;

#define MGBE_RDES3_OWN  0x80000000u
#define MGBE_RDES3_IOC  0x40000000u
#define MGBE_RDES3_CTXT 0x40000000u
#define MGBE_RDES3_FD   0x20000000u
#define MGBE_RDES3_LD   0x10000000u
#define MGBE_RDES3_ES   0x00008000u   /* error summary (MGBE) */
#define MGBE_RDES3_PKTLEN_MASK 0x00007FFFu

#define MGBE_TDES2_IOC  0x80000000u
#define MGBE_TDES3_OWN  0x80000000u
#define MGBE_TDES3_FD   0x20000000u
#define MGBE_TDES3_LD   0x10000000u

#define DMA_BUFFER_PAD 2
#define MGBE_CHAN 0

#define TX_RING_SIZE 64
#define TX_RING_MASK (TX_RING_SIZE - 1)
#define RX_RING_SIZE 128
#define RX_RING_MASK (RX_RING_SIZE - 1)

typedef struct t234_mgbe {
  ether_netif_t me_eni;

  uint64_t me_mac;    /* MAC region base  */
  uint64_t me_xpcs;   /* XPCS region base */

  desc_t *me_txring;
  desc_t *me_rxring;

  void *me_tx_pbuf_data[TX_RING_SIZE];
  void *me_rx_pbuf_data[RX_RING_SIZE];

  uint8_t me_next_rx;
  uint8_t me_tx_rdptr;
  uint8_t me_tx_wrptr;

  uint8_t me_phyaddr;
} t234_mgbe_t;

static t234_mgbe_t g_mgbe0;

static error_t mgbe_output(struct ether_netif *eni, pbuf_t *pb,
                           pbuf_tx_cb_t *txcb, uint32_t id);
static void mgbe_irq(void *arg);

/* ------------------------------------------------------------------ */
/* register helpers (offsets relative to a region base)               */
/* ------------------------------------------------------------------ */

static inline uint32_t mac_rd(t234_mgbe_t *me, uint32_t off)
{ return reg_rd(me->me_mac + off); }
static inline void mac_wr(t234_mgbe_t *me, uint32_t off, uint32_t v)
{ reg_wr(me->me_mac + off, v); }

/* ------------------------------------------------------------------ */
/* XPCS access                                                        */
/* ------------------------------------------------------------------ */

static inline uint32_t xpcs_wrap_rd(t234_mgbe_t *me, uint32_t off)
{ return reg_rd(me->me_xpcs + off); }
static inline void xpcs_wrap_wr(t234_mgbe_t *me, uint32_t off, uint32_t v)
{ reg_wr(me->me_xpcs + off, v); }

static uint32_t
xpcs_rd(t234_mgbe_t *me, uint32_t reg)
{
  reg_wr(me->me_xpcs + XPCS_ADDRESS,
         (reg >> XPCS_REG_ADDR_SHIFT) & XPCS_REG_ADDR_MASK);
  return reg_rd(me->me_xpcs + (reg & XPCS_REG_VALUE_MASK));
}

static void
xpcs_wr(t234_mgbe_t *me, uint32_t reg, uint32_t val)
{
  reg_wr(me->me_xpcs + XPCS_ADDRESS,
         (reg >> XPCS_REG_ADDR_SHIFT) & XPCS_REG_ADDR_MASK);
  reg_wr(me->me_xpcs + (reg & XPCS_REG_VALUE_MASK), val);
}

static error_t
xpcs_wr_safe(t234_mgbe_t *me, uint32_t reg, uint32_t val)
{
  for(int i = 0; i < 10; i++) {
    xpcs_wr(me, reg, val);
    if(xpcs_rd(me, reg) == val)
      return 0;
    usleep(1);
  }
  return ERR_NOT_READY;
}

/* Bring up the UPHY Tx lane via the XPCS FSM wrapper (xpcs.c). */
static error_t
xpcs_uphy_tx_lane(t234_mgbe_t *me)
{
  if((xpcs_wrap_rd(me, XPCS_WRAP_UPHY_STATUS) &
      XPCS_WRAP_UPHY_STATUS_TX_P_UP) == XPCS_WRAP_UPHY_STATUS_TX_P_UP)
    return 0;

  uint32_t v = xpcs_wrap_rd(me, XPCS_WRAP_UPHY_HW_INIT_CTRL);
  v |= XPCS_WRAP_UPHY_HW_INIT_CTRL_TX_EN;
  xpcs_wrap_wr(me, XPCS_WRAP_UPHY_HW_INIT_CTRL, v);

  for(int i = 0; i < 6; i++) {
    v = xpcs_wrap_rd(me, XPCS_WRAP_UPHY_HW_INIT_CTRL);
    if((v & XPCS_WRAP_UPHY_HW_INIT_CTRL_TX_EN) == 0)
      return 0;
    usleep(1);
  }
  return ERR_NOT_READY;
}

static void
rx_ctrl_rmw(t234_mgbe_t *me, uint32_t set, uint32_t clr)
{
  uint32_t v = xpcs_wrap_rd(me, XPCS_WRAP_UPHY_RX_CONTROL_0_0);
  v |= set;
  v &= ~clr;
  xpcs_wrap_wr(me, XPCS_WRAP_UPHY_RX_CONTROL_0_0, v);
}

/* UPHY Rx lane bring-up + PCS block lock (xpcs_lane_bring_up). */
static error_t
xpcs_lane_bring_up(t234_mgbe_t *me)
{
  if(xpcs_uphy_tx_lane(me)) {
    evlog(LOG_ERR, "%s: UPHY TX lane bring-up failed", MAC_NAME);
    return ERR_NOT_READY;
  }

  rx_ctrl_rmw(me, XPCS_RX_SW_OVRD, 0);   /* step1 */
  rx_ctrl_rmw(me, 0, XPCS_RX_IDDQ);      /* step2 */
  rx_ctrl_rmw(me, 0, XPCS_AUX_RX_IDDQ);
  rx_ctrl_rmw(me, 0, XPCS_RX_SLEEP);     /* step3 */
  rx_ctrl_rmw(me, XPCS_RX_CAL_EN, 0);    /* step4 */

  int i;
  for(i = 0; i < 7; i++) {               /* step5: poll cal complete */
    if((xpcs_wrap_rd(me, XPCS_WRAP_UPHY_RX_CONTROL_0_0) &
        XPCS_RX_CAL_EN) == 0)
      break;
    usleep(14);
  }
  if(i == 7) {
    evlog(LOG_ERR, "%s: UPHY RX cal timeout (rxctrl=0x%08x)", MAC_NAME,
          xpcs_wrap_rd(me, XPCS_WRAP_UPHY_RX_CONTROL_0_0));
    return ERR_NOT_READY;
  }

  rx_ctrl_rmw(me, XPCS_RX_DATA_EN, 0);     /* step6 */
  rx_ctrl_rmw(me, XPCS_RX_CDR_RESET, 0);   /* step7 */
  rx_ctrl_rmw(me, 0, XPCS_RX_CDR_RESET);   /* step8 */
  rx_ctrl_rmw(me, XPCS_RX_PCS_PHY_RDY, 0); /* step9 */

  /* Poll for PCS block lock. This needs the AQR's line side (cable + partner)
   * up and its firmware initialised, so it can take a couple of seconds or
   * never (no cable). The caller retries, so keep the per-attempt window
   * short (a few seconds) rather than the ~1ms the OSI reference allows. */
  for(i = 0; i < 3000; i++) {
    uint32_t v = xpcs_wrap_rd(me, XPCS_WRAP_IRQ_STATUS);
    if(v & XPCS_WRAP_IRQ_STATUS_PCS_LINK_STS) {
      xpcs_wrap_wr(me, XPCS_WRAP_IRQ_STATUS, v); /* clear */
      if(i)
        evlog(LOG_INFO, "%s: PCS block lock after %d ms", MAC_NAME, i);
      return 0;
    }
    usleep(1000);
  }
  /* Expected transient while the line side is still coming up; the caller
   * retries. Logged at debug level (with state) rather than as an error. */
  evlog(LOG_DEBUG, "%s: PCS block lock not yet "
        "(uphy_status=0x%08x irq_status=0x%08x rxctrl=0x%08x)", MAC_NAME,
        xpcs_wrap_rd(me, XPCS_WRAP_UPHY_STATUS),
        xpcs_wrap_rd(me, XPCS_WRAP_IRQ_STATUS),
        xpcs_wrap_rd(me, XPCS_WRAP_UPHY_RX_CONTROL_0_0));
  return ERR_NOT_READY;
}

/*
 * XPCS init for XFI 10G (xpcs_init). The USXGMII-specific steps
 * (KR_CTRL USXG_MODE, AN_EN clear, CL37_BP) are skipped: the devkit
 * onboard port is phy-iface-mode XFI (DT value 0), not USXGMII.
 * TODO(hw): confirm phy-iface-mode on the production board.
 */
static error_t
xpcs_init(t234_mgbe_t *me)
{
  error_t err = xpcs_lane_bring_up(me);
  if(err)
    return err;

  uint32_t ctrl;

  /* 1. switch DWC_xpcs to BASE-R mode */
  ctrl = xpcs_rd(me, XPCS_SR_XS_PCS_CTRL2);
  ctrl |= XPCS_SR_XS_PCS_CTRL2_TYPE_BASE_R;
  if((err = xpcs_wr_safe(me, XPCS_SR_XS_PCS_CTRL2, ctrl)))
    return err;

  /* 5. vendor-specific software reset */
  ctrl = xpcs_rd(me, XPCS_VR_XS_PCS_DIG_CTRL1);
  ctrl |= XPCS_VR_XS_PCS_DIG_CTRL1_USXG_EN;
  if((err = xpcs_wr_safe(me, XPCS_VR_XS_PCS_DIG_CTRL1, ctrl)))
    return err;

  ctrl |= XPCS_VR_XS_PCS_DIG_CTRL1_VR_RST;  /* self-clearing */
  xpcs_wr(me, XPCS_VR_XS_PCS_DIG_CTRL1, ctrl);

  for(int i = 0; i < 1000; i++) {
    if((xpcs_rd(me, XPCS_VR_XS_PCS_DIG_CTRL1) &
        XPCS_VR_XS_PCS_DIG_CTRL1_VR_RST) == 0)
      return 0;
    usleep(1000);
  }
  return ERR_NOT_READY;
}

/* ------------------------------------------------------------------ */
/* MDIO (Clause 45)                                                   */
/* ------------------------------------------------------------------ */

static error_t
mdio_busy_wait(t234_mgbe_t *me)
{
  for(int i = 0; i < 1000; i++) {
    if((mac_rd(me, MGBE_MAC_MDIO_SCCD) & MGBE_MDIO_SCCD_SBUSY) == 0)
      return 0;
    if(can_sleep())
      usleep(10);
  }
  return ERR_NOT_READY;
}

/*
 * Clause-45 MDIO. The 16-bit "reg" carries the device address in bits
 * [20:16] and the register address in [15:0], matching how the OSI layer
 * encodes phyreg. A Clause-45 PHY driver must pass (devad << 16) | regaddr.
 */
static int
mdio_read(t234_mgbe_t *me, uint32_t phyreg)
{
  if(mdio_busy_wait(me))
    return ERR_NOT_READY;

  mac_wr(me, MGBE_MAC_MDIO_SCCA,
         (((phyreg >> 16) & 0x1F) << MGBE_MDIO_SCCA_DA_SHIFT) |
         (me->me_phyaddr << MGBE_MDIO_SCCA_PA_SHIFT) |
         (phyreg & 0xFFFF));

  mac_wr(me, MGBE_MAC_MDIO_SCCD,
         (MGBE_MDIO_SCCD_CMD_RD << MGBE_MDIO_SCCD_CMD_SHIFT) |
         (0x5u << MGBE_MDIO_SCCD_CR_SHIFT) |
         MGBE_MDIO_SCCD_SBUSY);

  if(mdio_busy_wait(me))
    return ERR_NOT_READY;

  return mac_rd(me, MGBE_MAC_MDIO_SCCD) & 0xFFFF;
}

static error_t
mdio_write(t234_mgbe_t *me, uint32_t phyreg, uint16_t val)
{
  if(mdio_busy_wait(me))
    return ERR_NOT_READY;

  mac_wr(me, MGBE_MAC_MDIO_SCCA,
         (((phyreg >> 16) & 0x1F) << MGBE_MDIO_SCCA_DA_SHIFT) |
         (me->me_phyaddr << MGBE_MDIO_SCCA_PA_SHIFT) |
         (phyreg & 0xFFFF));

  mac_wr(me, MGBE_MAC_MDIO_SCCD,
         val |
         (MGBE_MDIO_SCCD_CMD_WR << MGBE_MDIO_SCCD_CMD_SHIFT) |
         (0x5u << MGBE_MDIO_SCCD_CR_SHIFT) |
         MGBE_MDIO_SCCD_SBUSY);

  return mdio_busy_wait(me);
}

static int
edc_mii_read(ether_netif_t *eni, uint16_t reg)
{
  return mdio_read((t234_mgbe_t *)eni, reg);
}

static error_t
edc_mii_write(ether_netif_t *eni, uint16_t reg, uint16_t value)
{
  return mdio_write((t234_mgbe_t *)eni, reg, value);
}

static void
mgbe_set_link_params(ether_netif_t *eni, int speed, int full_duplex)
{
  /* No-op: the AQR113C rate-adapts, so the MAC<->PHY (host) link is always
   * 10GBASE-R regardless of the line-side speed negotiated with the switch
   * (confirmed: a 1G switch links at line rate with the MAC left at 10G). */
}

static void
mgbe_print_info(struct device *dev, struct stream *st)
{
  ether_print((ether_netif_t *)dev, st);
}

/*
 * Quiesce MGBE before the cpubl hands control to the kernel (device_shutdown()
 * in efiboot.c). We ran the DMA engine for netboot with descriptors pointing at
 * cpubl memory; once the kernel reclaims those pages a still-running MGBE would
 * DMA into them and corrupt the kernel (seen as mgbeawr MC VPR/route-sanity
 * violations + a random early-boot oops). Stop RX/TX at both the MAC and the
 * DMA channel and mask the interrupt so the block is idle at handoff; the
 * kernel's nvethernet driver resets and reinitialises it from scratch.
 */
static error_t
mgbe_shutdown(struct device *dev)
{
  t234_mgbe_t *me = (t234_mgbe_t *)dev;
  const int ch = MGBE_CHAN;

  mac_wr(me, MGBE_MAC_RMCR, mac_rd(me, MGBE_MAC_RMCR) & ~MGBE_MAC_RMCR_RE);
  mac_wr(me, MGBE_MAC_TMCR, mac_rd(me, MGBE_MAC_TMCR) & ~MGBE_MAC_TMCR_TE);
  mac_wr(me, MGBE_DMA_CHX_RX_CTRL(ch),
         mac_rd(me, MGBE_DMA_CHX_RX_CTRL(ch)) & ~MGBE_DMA_CHX_RX_CTRL_SR);
  mac_wr(me, MGBE_DMA_CHX_TX_CTRL(ch),
         mac_rd(me, MGBE_DMA_CHX_TX_CTRL(ch)) & ~MGBE_DMA_CHX_TX_CTRL_ST);
  mac_wr(me, MGBE_DMA_CHX_INTR_ENA(ch), 0);
  mac_wr(me, MGBE_VIRT_INTR_CHX_CNTRL(ch), 0);
  asm volatile("dsb sy");
  return 0;
}

static const ethmac_device_class_t mgbe_device_class = {
  .dc = {
    .dc_class_name = MAC_NAME,
    .dc_print_info = mgbe_print_info,
    .dc_shutdown = mgbe_shutdown,
  },
  .edc_mii_read = edc_mii_read,
  .edc_mii_write = edc_mii_write,
  .edc_set_link_params = mgbe_set_link_params,
};

/* ------------------------------------------------------------------ */
/* RX/TX descriptor handling                                          */
/* ------------------------------------------------------------------ */

static void
rx_desc_give(t234_mgbe_t *me, size_t index, void *buf)
{
  assert(buf != NULL);
  me->me_rx_pbuf_data[index] = buf;

  void *dmabuf = buf + DMA_BUFFER_PAD;
  cache_op(dmabuf, PBUF_DATA_SIZE - DMA_BUFFER_PAD, DCACHE_INVALIDATE);

  desc_t *rx = me->me_rxring + index;
  uint64_t pa = (uintptr_t)dmabuf;
  rx->w0 = (uint32_t)pa;
  rx->w1 = (uint32_t)(pa >> 32);
  rx->w2 = 0;
  asm volatile("dsb sy");
  rx->w3 = MGBE_RDES3_OWN | MGBE_RDES3_IOC;
  asm volatile("dsb sy");
  mac_wr(me, MGBE_DMA_CHX_RDTLP(MGBE_CHAN), (uint32_t)(uintptr_t)rx);
}

static void
handle_irq_rx(t234_mgbe_t *me)
{
  while(1) {
    size_t idx = me->me_next_rx & RX_RING_MASK;
    desc_t *rx = me->me_rxring + idx;
    const uint32_t w3 = rx->w3;
    if(w3 & MGBE_RDES3_OWN)
      break;

    void *buf = me->me_rx_pbuf_data[idx];
    assert(buf != NULL);

    /* Only single-buffer frames are handled (RX buffer >= MTU). */
    const int complete = (w3 & (MGBE_RDES3_FD | MGBE_RDES3_LD)) ==
      (MGBE_RDES3_FD | MGBE_RDES3_LD);
    const int err = (w3 & MGBE_RDES3_ES) || (w3 & MGBE_RDES3_CTXT);
    const int len = w3 & MGBE_RDES3_PKTLEN_MASK;

    if(complete && !err) {
      /* Invalidate the just-DMA'd frame before the stack reads it, so we get
       * the fresh DMA data and not speculatively-cached stale lines. (We also
       * invalidate before handing the buffer to HW in rx_desc_give; both are
       * needed on a speculating core with cached DMA buffers.) */
      cache_op((uint8_t *)buf + DMA_BUFFER_PAD, len, DCACHE_INVALIDATE);
      pbuf_t *pb = pbuf_get(0);
      if(pb != NULL) {
        void *nextbuf = pbuf_data_get(0);
        if(nextbuf != NULL) {
          pb->pb_data = buf;
          pb->pb_offset = DMA_BUFFER_PAD;
          pb->pb_flags = PBUF_SOP | PBUF_EOP;
          pb->pb_buflen = len;
          pb->pb_pktlen = len;
          STAILQ_INSERT_TAIL(&me->me_eni.eni_ni.ni_rx_queue, pb, pb_link);
          netif_wakeup(&me->me_eni.eni_ni);
          me->me_eni.eni_stats.rx_pkt++;
          me->me_eni.eni_stats.rx_byte += len;
          buf = nextbuf;
        } else {
          me->me_eni.eni_stats.rx_sw_qdrop++;
          pbuf_put(pb);
        }
      }
    } else {
      me->me_eni.eni_stats.rx_other_err++;
    }

    rx_desc_give(me, idx, buf);
    me->me_next_rx++;
  }
}

static void
handle_irq_tx(t234_mgbe_t *me)
{
  while(me->me_tx_rdptr != me->me_tx_wrptr) {
    const size_t rdptr = me->me_tx_rdptr & TX_RING_MASK;
    desc_t *tx = me->me_txring + rdptr;
    if(tx->w3 & MGBE_TDES3_OWN)
      break;

    pbuf_data_put(me->me_tx_pbuf_data[rdptr]);
    me->me_tx_pbuf_data[rdptr] = NULL;
    me->me_tx_rdptr++;
  }
}

static error_t
mgbe_output(struct ether_netif *eni, pbuf_t *pb,
            pbuf_tx_cb_t *txcb, uint32_t id)
{
  t234_mgbe_t *me = (t234_mgbe_t *)eni;
  pbuf_t *n;
  size_t count = 0;

  for(n = pb; n != NULL; n = STAILQ_NEXT(n, pb_link))
    count++;

  int q = irq_forbid(IRQ_LEVEL_NET);
  int wrptr = me->me_tx_wrptr;
  const uint8_t qlen = (wrptr - me->me_tx_rdptr) & 0xff;

  if(qlen + count >= TX_RING_SIZE) {
    pbuf_free_irq_blocked(pb);
    eni->eni_stats.tx_qdrop++;
    irq_permit(q);
    return ERR_QUEUE_FULL;
  }

  eni->eni_stats.tx_pkt++;
  eni->eni_stats.tx_byte += pb->pb_pktlen;

  for(; pb != NULL; pb = n) {
    n = STAILQ_NEXT(pb, pb_link);

    desc_t *tx = me->me_txring + (wrptr & TX_RING_MASK);
    void *dmabuf = pb->pb_data + pb->pb_offset;
    cache_op(dmabuf, pb->pb_buflen, DCACHE_CLEAN | DCACHE_INVALIDATE);
    me->me_tx_pbuf_data[wrptr & TX_RING_MASK] = pb->pb_data;

    uint64_t pa = (uintptr_t)dmabuf;
    tx->w0 = (uint32_t)pa;
    tx->w1 = (uint32_t)(pa >> 32);
    tx->w2 = MGBE_TDES2_IOC | pb->pb_buflen;

    uint32_t w3 = MGBE_TDES3_OWN | pb->pb_pktlen;
    if(pb->pb_flags & PBUF_SOP)
      w3 |= MGBE_TDES3_FD;
    if(pb->pb_flags & PBUF_EOP)
      w3 |= MGBE_TDES3_LD;

    /* TX timestamp callbacks (PTP) are not supported on this boot path;
     * free the pbuf header now, free the data buffer on completion. */
    pbuf_put(pb);

    asm volatile("dsb sy");
    tx->w3 = w3;
    wrptr++;
  }

  desc_t *tx = me->me_txring + (wrptr & TX_RING_MASK);
  asm volatile("dsb sy");
  mac_wr(me, MGBE_DMA_CHX_TDTLP(MGBE_CHAN), (uint32_t)(uintptr_t)tx);

  me->me_tx_wrptr = wrptr;
  irq_permit(q);
  return 0;
}

static void
mgbe_irq(void *arg)
{
  t234_mgbe_t *me = arg;

  const uint32_t status = mac_rd(me, MGBE_DMA_CHX_STATUS(MGBE_CHAN));

  if(status & MGBE_DMA_CHX_STATUS_TI)
    handle_irq_tx(me);
  if(status & MGBE_DMA_CHX_STATUS_RI)
    handle_irq_rx(me);

  /* W1C the handled bits + the normal-summary bit */
  mac_wr(me, MGBE_DMA_CHX_STATUS(MGBE_CHAN),
         status & (MGBE_DMA_CHX_STATUS_TI | MGBE_DMA_CHX_STATUS_RI |
                   MGBE_DMA_CHX_STATUS_NIS));
  /* clear the Tegra wrapper status */
  mac_wr(me, MGBE_VIRT_INTR_CHX_STATUS(MGBE_CHAN),
         MGBE_VIRT_INTR_TX | MGBE_VIRT_INTR_RX);
}

/* ------------------------------------------------------------------ */
/* Bring-up                                                           */
/* ------------------------------------------------------------------ */

/* Drive a Tegra234 GPIO as a push-pull output at the given level. */
static void
gpio_drive(uint64_t pinbase, int high)
{
  reg_wr(pinbase + T234_GPIO_OUTPUT_VALUE,
         high ? T234_GPIO_OUTPUT_VALUE_HIGH : 0);
  reg_wr(pinbase + T234_GPIO_OUTPUT_CONTROL, 0); /* clear FLOATED -> driven */
  reg_wr(pinbase + T234_GPIO_ENABLE_CONFIG,
         T234_GPIO_ENABLE_CONFIG_ENABLE | T234_GPIO_ENABLE_CONFIG_OUT);
}

/*
 * Reset-cycle the AQR113C. NVIDIA (ether_linux.c) drives the line high,
 * asserts low for ~221ms, releases high, then waits ~150ms. Without this
 * the PHY host-side XFI never comes up and the XPCS gets no block lock.
 * Relies on MB1 pinmux leaving PY.01 as a GPIO owned by CCPLEX (confirmed on
 * the AGX devkit; verify on other boards).
 */
static void
mgbe_phy_reset(void)
{
  gpio_drive(MGBE0_PHY_RESET_GPIO, 1);
  gpio_drive(MGBE0_PHY_RESET_GPIO, 0);
  usleep(MGBE0_PHY_RST_DUR_US);
  gpio_drive(MGBE0_PHY_RESET_GPIO, 1);
  usleep(MGBE0_PHY_RST_PDELAY_US);
}

static void
mgbe_clocks_on(void)
{
  /* MB2/BPMP already enables most of these; we enable the full set
   * defensively (re-enabling an already-on clock is harmless). */
  reset_peripheral(RESET_MGBE0_MAC);
  reset_peripheral(RESET_MGBE0_PCS);

  clk_enable(CLK_MGBE0_APP);
  clk_enable(CLK_MGBE0_MAC);
  clk_enable(CLK_MGBE0_MAC_DIVIDER);
  clk_enable(CLK_MGBE0_TX);
  clk_enable(CLK_MGBE0_TX_PCS);
  clk_enable(CLK_MGBE0_RX_INPUT);
  clk_enable(CLK_MGBE0_RX_INPUT_M);
  clk_enable(CLK_MGBE0_RX_PCS_INPUT);
  clk_enable(CLK_MGBE0_RX_PCS);
  clk_enable(CLK_MGBE0_RX_PCS_M);
  clk_enable(CLK_MGBE0_PTP_REF);
  clk_enable(CLK_MGBE0_EEE_PCS);
}

static error_t
mgbe_mac_init(t234_mgbe_t *me)
{
  /* Software reset (DMA_MODE SWR), self-clearing once clocks are live. */
  mac_wr(me, MGBE_DMA_MODE, mac_rd(me, MGBE_DMA_MODE) | MGBE_DMA_MODE_SWR);
  int i;
  for(i = 0; i < 100; i++) {
    if((mac_rd(me, MGBE_DMA_MODE) & MGBE_DMA_MODE_SWR) == 0)
      break;
    usleep(10);
  }
  if(i == 100) {
    evlog(LOG_ERR, "%s: MAC reset timeout (no clock?)", MAC_NAME);
    return ERR_NOT_READY;
  }

  /* RxQ0 -> DMA channel 0 mapping */
  mac_wr(me, MGBE_MTL_RXQ_DMA_MAP0, MGBE_RXQ_TO_DMA_MAP0);

  /* MTL Tx queue 0: store-and-forward + enable */
  mac_wr(me, MGBE_MTL_TX_OP_MODE(0),
         (MGBE_TXQ0_FIFO_SZ << MGBE_MTL_TXQ_SIZE_SHIFT) |
         MGBE_MTL_TSF | MGBE_MTL_TXQEN);

  /* MTL Rx queue 0: store-and-forward */
  mac_wr(me, MGBE_MTL_RX_OP_MODE(0),
         (MGBE_RXQ0_FIFO_SZ << MGBE_MTL_RXQ_SIZE_SHIFT) | MGBE_MTL_RSF);

  /* Enable RxQ0 for DCB/generic traffic (value 2) */
  mac_wr(me, MGBE_MAC_RQC0R, MGBE_MAC_RXQC0_RXQEN(0, 2));

  /* Rx config: auto-pad/CRC strip + Rx checksum offload */
  mac_wr(me, MGBE_MAC_RMCR,
         mac_rd(me, MGBE_MAC_RMCR) |
         MGBE_MAC_RMCR_ACS | MGBE_MAC_RMCR_CST | MGBE_MAC_RMCR_IPC);

  mac_wr(me, MGBE_MAC_TMCR,
         mac_rd(me, MGBE_MAC_TMCR) | MGBE_MAC_TMCR_DDIC);

  /* Send multicast/broadcast to queue 0 */
  mac_wr(me, MGBE_MAC_RQC1R,
         (mac_rd(me, MGBE_MAC_RQC1R) | MGBE_MAC_RQC1R_MCBCQEN) &
         ~(0xfu << MGBE_MAC_RQC1R_MCBCQ_SHIFT));

  /* Promiscuous (Receive All), same as the reference Synopsys driver
   * stm32h7_eth.c. MGBE perfect filtering would additionally need per-channel
   * routing for unicast (XDCS table) AND broadcast/multicast, which is not
   * worth it for a boot-time-only driver. */
  mac_wr(me, MGBE_MAC_PFR, MGBE_MAC_PFR_RA);

  /* Program MAC address 0 (used as TX source address). */
  const uint8_t *a = me->me_eni.eni_addr;
  mac_wr(me, MGBE_MAC_ADDRL(0),
         a[0] | (a[1] << 8) | (a[2] << 16) | (a[3] << 24));
  mac_wr(me, MGBE_MAC_ADDRH(0),
         MGBE_MAC_ADDRH_AE | a[4] | (a[5] << 8));

  /* DMA system-bus mode */
  mac_wr(me, MGBE_DMA_SBUS,
         MGBE_DMA_SBUS_UNDEF | MGBE_DMA_SBUS_BLEN256 | MGBE_DMA_SBUS_EAME);

  return 0;
}

static void
mgbe_dma_init(t234_mgbe_t *me)
{
  const int ch = MGBE_CHAN;
  uint64_t txpa = (uintptr_t)me->me_txring;
  uint64_t rxpa = (uintptr_t)me->me_rxring;

  mac_wr(me, MGBE_DMA_CHX_CTRL(ch), MGBE_DMA_CHX_CTRL_PBLX8);

  /* Tx ring */
  mac_wr(me, MGBE_DMA_CHX_TDLH(ch), (uint32_t)(txpa >> 32));
  mac_wr(me, MGBE_DMA_CHX_TDLA(ch), (uint32_t)txpa);
  mac_wr(me, MGBE_DMA_CHX_TX_CNTRL2(ch),
         (mac_rd(me, MGBE_DMA_CHX_TX_CNTRL2(ch)) & ~MGBE_DMA_RING_LEN_MASK) |
         ((TX_RING_SIZE - 1) & MGBE_DMA_RING_LEN_MASK));
  mac_wr(me, MGBE_DMA_CHX_TDTLP(ch), (uint32_t)txpa);

  /* Rx ring */
  mac_wr(me, MGBE_DMA_CHX_RDLH(ch), (uint32_t)(rxpa >> 32));
  mac_wr(me, MGBE_DMA_CHX_RDLA(ch), (uint32_t)rxpa);
  mac_wr(me, MGBE_DMA_CHX_RX_CNTRL2(ch),
         (mac_rd(me, MGBE_DMA_CHX_RX_CNTRL2(ch)) & ~MGBE_DMA_RING_LEN_MASK) |
         ((RX_RING_SIZE - 1) & MGBE_DMA_RING_LEN_MASK));

  /* Rx buffer size (RBSZ, [14:1], multiple of 4) */
  const uint32_t rbsz = (PBUF_DATA_SIZE - DMA_BUFFER_PAD) & ~3u;
  mac_wr(me, MGBE_DMA_CHX_RX_CTRL(ch),
         (mac_rd(me, MGBE_DMA_CHX_RX_CTRL(ch)) & ~(0x7FFEu)) |
         (rbsz << MGBE_DMA_CHX_RX_CTRL_RBSZ_SHIFT));

  mac_wr(me, MGBE_DMA_CHX_TX_CTRL(ch),
         mac_rd(me, MGBE_DMA_CHX_TX_CTRL(ch)) | MGBE_DMA_CHX_TX_CTRL_OSP);

  /* Per-channel + Tegra-wrapper interrupt enables */
  mac_wr(me, MGBE_DMA_CHX_INTR_ENA(ch),
         MGBE_DMA_CHX_INTR_NIE | MGBE_DMA_CHX_INTR_TIE | MGBE_DMA_CHX_INTR_RIE);
  mac_wr(me, MGBE_VIRT_INTR_CHX_CNTRL(ch),
         MGBE_VIRT_INTR_TX | MGBE_VIRT_INTR_RX);
  /* Route this channel's interrupt to VM0's GIC line (vm0). Without this the
   * wrapper latches STATUS but asserts no GIC SPI (ISPENDR stays 0). */
  mac_wr(me, MGBE_VIRT_INTR_APB_CHX_CNTRL(ch), 1u /* BIT(vm_num=0) */);

  /* Start DMA */
  mac_wr(me, MGBE_DMA_CHX_TX_CTRL(ch),
         mac_rd(me, MGBE_DMA_CHX_TX_CTRL(ch)) | MGBE_DMA_CHX_TX_CTRL_ST);
  mac_wr(me, MGBE_DMA_CHX_RX_CTRL(ch),
         mac_rd(me, MGBE_DMA_CHX_RX_CTRL(ch)) | MGBE_DMA_CHX_RX_CTRL_SR);
}

/*
 * Link poll for the AQR113C. It is Clause-45 only, so the generic ethphy
 * driver (Clause-22 BMSR) cannot read it. Poll the line-side link directly:
 * PMA/PMD (dev 1) STAT1 bit2 is the receive-link bit, latched-low, so read
 * twice and use the second value. Drive the netif up/down, which is what
 * starts/stops DHCP. The host-side XPCS link is already up; this tracks
 * whether the cable/line side has a partner.
 */
#define AQR_MMD_PMA         1
#define MDIO_STAT1          1
#define MDIO_STAT1_LSTATUS  (1u << 2)

static void __attribute__((noreturn))
mgbe_link_poll(t234_mgbe_t *me)
{
  /*
   * We only reach here after PCS block lock, which requires the line side to
   * be up, so start in the up state and announce it - this avoids the
   * spurious startup down->up caused by the PMA link bit (latched-low)
   * lagging block lock by a poll or two. Then track changes, requiring two
   * consecutive matching readings before acting so a single latched/transient
   * glitch can't bounce the netif (and DHCP) up and down.
   */
  net_task_raise(&me->me_eni.eni_ni.ni_task, NETIF_TASK_STATUS_UP);
  int state = 1;     /* announced link state */
  int differ = 0;    /* consecutive reads differing from state */

  while(1) {
    usleep(200000);

    mdio_read(me, (AQR_MMD_PMA << 16) | MDIO_STAT1);  /* clear latch */
    int s = mdio_read(me, (AQR_MMD_PMA << 16) | MDIO_STAT1);
    int up = (s >= 0) && (s & MDIO_STAT1_LSTATUS);

    if(up == state) {
      differ = 0;
      continue;
    }
    if(++differ < 2)
      continue;  /* debounce: need two consecutive differing reads */

    state = up;
    differ = 0;
    evlog(LOG_INFO, "%s: line link %s", MAC_NAME, state ? "up" : "down");
    net_task_raise(&me->me_eni.eni_ni.ni_task,
                   state ? NETIF_TASK_STATUS_UP : NETIF_TASK_STATUS_DOWN);
  }
}

static void *
mgbe_thread(void *arg)
{
  t234_mgbe_t *me = arg;

  /* Bring the AQR113C out of reset before expecting a PCS link. */
  mgbe_phy_reset();

  mgbe_clocks_on();

  /* Diagnostic: can we reach the PHY over MDIO? C45 PMA/PMD (dev 1) holds
   * the PHY identifier in registers 2/3. A sane value means the PHY is
   * powered, out of reset and MDIO works; 0xffff/0x0000 means it is not. */
  evlog(LOG_INFO, "%s: MDIO probe addr0 dev1: id2=0x%04x id3=0x%04x",
        MAC_NAME,
        mdio_read(me, (1u << 16) | 2),
        mdio_read(me, (1u << 16) | 3));

  /*
   * PCS/UPHY first: the MAC needs a live Rx clock from the PCS. Block lock
   * needs the AQR's line side (cable + partner) up, and that timing varies,
   * so retry instead of giving up after one window. This also means a cable
   * connected after boot brings the link up rather than leaving the driver
   * dead. (Runtime unplug/replug after this point is handled by the link
   * poll, since the configured XPCS re-locks on its own.)
   */
  for(int attempt = 0; xpcs_init(me); attempt++) {
    if((attempt % 8) == 0)
      evlog(LOG_INFO, "%s: waiting for PCS link (is a cable connected?)",
            MAC_NAME);
    usleep(500000);
  }
  evlog(LOG_INFO, "%s: PCS link up (10G)", MAC_NAME);

  if(mgbe_mac_init(me)) {
    thread_exit(0);
  }

  mgbe_dma_init(me);

  /* Enable MAC Tx/Rx */
  mac_wr(me, MGBE_MAC_TMCR, mac_rd(me, MGBE_MAC_TMCR) | MGBE_MAC_TMCR_TE);
  mac_wr(me, MGBE_MAC_RMCR, mac_rd(me, MGBE_MAC_RMCR) | MGBE_MAC_RMCR_RE);

  me->me_eni.eni_output = mgbe_output;
  /* No TX checksum offload: we don't program the descriptor CIC bits, so let
   * the stack compute IPv4/TCP/UDP checksums in software. */

  ether_netif_attach(&me->me_eni);

  /* Channel 0 routes to vm0 (see MGBE_VIRT_INTR_APB_CHX_CNTRL). */
  irq_enable_fn_arg(MGBE0_IRQ, IRQ_LEVEL_NET, mgbe_irq, me);

  /*
   * The AQR113C is Clause-45 only; do not attach the generic Clause-22
   * ethphy. Poll the line-side link ourselves and drive the netif. (noreturn)
   */
  mgbe_link_poll(me);
}

static void
mgbe_init(t234_mgbe_t *me, uint64_t mac_base, uint64_t xpcs_base,
          int phyaddr, const char *name)
{
  me->me_mac = mac_base;
  me->me_xpcs = xpcs_base;
  me->me_phyaddr = phyaddr;

  ether_netif_init(&me->me_eni, name, &mgbe_device_class);

  /* Locally-administered MAC. TODO(hw): read from board EEPROM/fuse. */
  me->me_eni.eni_addr[0] = 0x02;
  me->me_eni.eni_addr[1] = 0x00;
  me->me_eni.eni_addr[2] = 0x4d; /* 'M' */
  me->me_eni.eni_addr[3] = 0x47; /* 'G' */
  me->me_eni.eni_addr[4] = 0x42; /* 'B' */
  me->me_eni.eni_addr[5] = 0x00;

  /* Rings live in the Normal-NC heap (type NO_CACHE only). On t234ccplex
   * heaps are split by type and a request must be a subset of one heap's
   * type, so DMA|NO_CACHE matches neither pool (heap_simple malloc0). The
   * t234 PCIe NICs (rtl8168, lan743x) use NO_CACHE alone for the same reason. */
  me->me_txring = xalloc(sizeof(desc_t) * TX_RING_SIZE, CACHE_LINE_SIZE,
                         MEM_CLEAR | MEM_TYPE_NO_CACHE);
  me->me_rxring = xalloc(sizeof(desc_t) * RX_RING_SIZE, CACHE_LINE_SIZE,
                         MEM_CLEAR | MEM_TYPE_NO_CACHE);

  for(int i = 0; i < RX_RING_SIZE; i++) {
    void *buf = pbuf_data_get(0);
    if(buf == NULL)
      panic("mgbe: no pbufs");
    rx_desc_give(me, i, buf);
  }

  thread_create(mgbe_thread, me, 1024, "mgbe", TASK_NO_FPU, 4);
}

static void __attribute__((constructor(6100)))
t234ccplex_mgbe_init(void)
{
  mgbe_init(&g_mgbe0, MGBE0_MAC_BASE, MGBE0_XPCS_BASE, 0, "eth0");
}
