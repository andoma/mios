#include <mios/ethphy.h>
#include <mios/driver.h>
#include <mios/eventlog.h>

#include <net/ether.h>
#include <net/netif.h>

#include <stdio.h>
#include <unistd.h>

// Datasheet: TI DP83869HM (SNLS614D)

// Standard registers (directly accessible via MDIO)
#define REG_BMCR            0x0000  // Basic Mode Control
#define REG_BMSR            0x0001  // Basic Mode Status
#define REG_PHYIDR1         0x0002  // PHY Identifier 1
#define REG_PHYIDR2         0x0003  // PHY Identifier 2
#define REG_ANAR            0x0004  // Auto-Neg Advertisement
#define REG_ALNPAR          0x0005  // Auto-Neg LP Ability
#define REG_ANER            0x0006  // Auto-Neg Expansion
#define REG_GEN_CFG1        0x0009  // Configuration Register 1
#define REG_GEN_STATUS1     0x000a  // Status Register 1
#define REG_REGCR           0x000d  // Register Control
#define REG_ADDAR           0x000e  // Address or Data
#define REG_1KSCR           0x000f  // 1000BASE-T Status
#define REG_PHY_CONTROL     0x0010  // PHY Control
#define REG_PHY_STATUS      0x0011  // PHY Status
#define REG_INTERRUPT_MASK  0x0012  // Interrupt Mask
#define REG_INTERRUPT_STATUS 0x0013 // Interrupt Status
#define REG_GEN_CFG2        0x0014  // Configuration Register 2
#define REG_RX_ERR_CNT     0x0015  // Receive Error Counter
#define REG_GEN_STATUS2     0x0017  // Status Register 2
#define REG_LEDS_CFG1       0x0018  // LED Configuration 1
#define REG_GEN_CFG4        0x001e  // Configuration Register 4
#define REG_GEN_CTRL        0x001f  // Control Register

// Extended registers (accessed via REGCR/ADDAR)
#define REG_GEN_CFG3        0x0031  // Configuration Register 3
#define REG_RGMII_CTRL      0x0032  // RGMII Control
#define REG_RGMII_CTRL2     0x0033  // RGMII Control 2
#define REG_SGMII_AUTO_NEG_STATUS 0x0037
#define REG_STRAP_STS       0x006e  // Strap Status
#define REG_RGMII_DLL_CTRL  0x0086  // RGMII Delay Control
#define REG_IO_MUX_CFG      0x0170  // IO Mux Configuration
#define REG_OP_MODE_DECODE  0x01df  // Operation Mode Decode
#define REG_FX_CTRL         0x0c00  // Fiber Control
#define REG_FX_STS          0x0c01  // Fiber Status
#define REG_FX_PHYID1       0x0c02  // Fiber PHY ID 1
#define REG_FX_PHYID2       0x0c03  // Fiber PHY ID 2
#define REG_FX_ANAR         0x0c04  // Fiber Auto-Neg Advertisement
#define REG_FX_ANLPAR       0x0c05  // Fiber Auto-Neg LP Ability

// BMCR bits
#define BMCR_RESET          (1 << 15)
#define BMCR_LOOPBACK       (1 << 14)
#define BMCR_SPEED_SEL_LSB  (1 << 13)
#define BMCR_AUTONEG_EN     (1 << 12)
#define BMCR_POWER_DOWN     (1 << 11)
#define BMCR_ISOLATE        (1 << 10)
#define BMCR_RESTART_AUTONEG (1 << 9)
#define BMCR_DUPLEX_EN      (1 << 8)
#define BMCR_SPEED_SEL_MSB  (1 << 6)

// BMSR bits
#define BMSR_AUTONEG_COMP   (1 << 5)
#define BMSR_LINK_STS       (1 << 2)

// PHY_STATUS bits
#define PHY_STATUS_SPEED_SHIFT    14
#define PHY_STATUS_SPEED_MASK     (3 << PHY_STATUS_SPEED_SHIFT)
#define PHY_STATUS_DUPLEX         (1 << 13)
#define PHY_STATUS_RESOLVED       (1 << 11)
#define PHY_STATUS_LINK           (1 << 10)
#define PHY_STATUS_MDI_X_CD       (1 << 9)
#define PHY_STATUS_MDI_X_AB       (1 << 8)
#define PHY_STATUS_SLEEP          (1 << 6)

// OP_MODE_DECODE register
#define OP_MODE_DECODE_RGMII_MII_SEL (1 << 5)
#define OP_MODE_DECODE_CFG_OPMODE_MASK 0x07

// OP_MODE_DECODE CFG_OPMODE values
#define CFG_OPMODE_RGMII_COPPER     0
#define CFG_OPMODE_RGMII_1000BX     1
#define CFG_OPMODE_RGMII_100FX      2
#define CFG_OPMODE_RGMII_SGMII      3
#define CFG_OPMODE_1000BT_1000BX    4
#define CFG_OPMODE_100BT_100FX      5
#define CFG_OPMODE_SGMII_COPPER     6

// GEN_CTRL bits
#define GEN_CTRL_SW_RESET    (1 << 15)
#define GEN_CTRL_SW_RESTART  (1 << 14)

// RGMII_CTRL bits
#define RGMII_CTRL_TX_CLK_DELAY (1 << 1)
#define RGMII_CTRL_RX_CLK_DELAY (1 << 0)

typedef enum {
  DP83869_MEDIA_COPPER,
  DP83869_MEDIA_FIBER,
} dp83869_media_t;

static uint16_t
reg_read(struct ether_netif *eni, uint16_t reg)
{
  return ethphy_mii_read(eni, reg);
}

static void
reg_write(struct ether_netif *eni, uint16_t reg, uint16_t val)
{
  ethphy_mii_write(eni, reg, val);
}

static void
dp83869_set_rgmii_delay(struct ether_netif *eni, unsigned int flags)
{
  uint16_t rgmii_ctrl = reg_read(eni, REG_RGMII_CTRL);

  // The DELAY bits are inverted: 1 means off, 0 means on
  if(flags & ETHPHY_DELAY_RX)
    rgmii_ctrl &= ~RGMII_CTRL_RX_CLK_DELAY;
  else
    rgmii_ctrl |= RGMII_CTRL_RX_CLK_DELAY;

  if(flags & ETHPHY_DELAY_TX)
    rgmii_ctrl &= ~RGMII_CTRL_TX_CLK_DELAY;
  else
    rgmii_ctrl |= RGMII_CTRL_TX_CLK_DELAY;

  reg_write(eni, REG_RGMII_CTRL, rgmii_ctrl);
}


static void
dp83869_set_mode_copper(struct ether_netif *eni, ethphy_mode_t mode,
                        unsigned int flags)
{
  // Section 7.4.8.1: RGMII-to-Copper mode initialization
  reg_write(eni, REG_OP_MODE_DECODE, 0x0040);

  // Software Reset to apply mode change
  reg_write(eni, REG_GEN_CTRL, GEN_CTRL_SW_RESET);

  for(int i = 0; i < 1000; i++) {
    if(!(reg_read(eni, REG_GEN_CTRL) & GEN_CTRL_SW_RESET))
      break;
  }

  // Configure registers AFTER reset (reset wipes register state)
  reg_write(eni, REG_BMCR, 0x1140);
  reg_write(eni, REG_ANAR, 0x01e1);
  reg_write(eni, REG_GEN_CFG1, 0x0300);
  reg_write(eni, REG_PHY_CONTROL, 0x5048);

  if(mode == ETHPHY_MODE_RGMII) {
    uint16_t rgmii_ctrl = reg_read(eni, REG_RGMII_CTRL);

    // The DELAY bits are confusing because 1 means off, 0 means on
    if(flags & ETHPHY_DELAY_RX)
      rgmii_ctrl &= ~RGMII_CTRL_RX_CLK_DELAY;  // RX delay ON
    else
      rgmii_ctrl |= RGMII_CTRL_RX_CLK_DELAY;   // RX delay OFF

    if(flags & ETHPHY_DELAY_TX)
      rgmii_ctrl &= ~RGMII_CTRL_TX_CLK_DELAY;  // TX delay ON
    else
      rgmii_ctrl |= RGMII_CTRL_TX_CLK_DELAY;   // TX delay OFF

    reg_write(eni, REG_RGMII_CTRL, rgmii_ctrl);
  }
}

static void
dp83869_set_mode_fiber(struct ether_netif *eni, ethphy_mode_t mode,
                       unsigned int flags)
{
  // Explicitly set RGMII-to-1000Base-X mode (match Linux: 0x0041)
  reg_write(eni, REG_OP_MODE_DECODE, 0x0041);

  // Set FX_CTRL: auto-neg, full duplex, 1000M
  reg_write(eni, REG_FX_CTRL,
            BMCR_AUTONEG_EN | BMCR_DUPLEX_EN | BMCR_SPEED_SEL_MSB);

  // Soft reset sequence (from Linux driver)
  reg_write(eni, 0x00c6, 0x10);
  reg_write(eni, REG_RGMII_DLL_CTRL, 0x0077);

  if(mode == ETHPHY_MODE_RGMII)
    dp83869_set_rgmii_delay(eni, flags);

  // Configure IO mux for fiber signal detect
  reg_write(eni, REG_IO_MUX_CFG, 0x1f);

  // Reset FX_CTRL to apply (RESET bit is self-clearing)
  reg_write(eni, REG_FX_CTRL,
            BMCR_RESET | BMCR_AUTONEG_EN | BMCR_DUPLEX_EN |
            BMCR_SPEED_SEL_MSB);
}

static const char *
speed_str(uint16_t phy_status)
{
  switch((phy_status & PHY_STATUS_SPEED_MASK) >> PHY_STATUS_SPEED_SHIFT) {
  case 0: return "10M";
  case 1: return "100M";
  case 2: return "1000M";
  default: return "unknown";
  }
}

static const char *
opmode_str(uint16_t opmode)
{
  switch(opmode & OP_MODE_DECODE_CFG_OPMODE_MASK) {
  case 0: return "RGMII-to-Copper";
  case 1: return "RGMII-to-1000Base-X";
  case 2: return "RGMII-to-100Base-FX";
  case 3: return "RGMII-to-SGMII";
  case 4: return "1000Base-T-to-1000Base-X";
  case 5: return "100Base-TX-to-100Base-FX";
  case 6: return "SGMII-to-Copper";
  default: return "Reserved";
  }
}

static void
dp83869_print_diagnostics(struct device *dev, stream_t *s)
{
  struct ether_netif *eni = (struct ether_netif *)dev->d_parent;

  uint16_t id2 = reg_read(eni, REG_PHYIDR2);
  uint16_t opmode = reg_read(eni, REG_OP_MODE_DECODE);
  uint16_t cfg_opmode = opmode & OP_MODE_DECODE_CFG_OPMODE_MASK;

  stprintf(s, "DP83869HM rev %d, %s (%s)\n",
           id2 & 0xf, opmode_str(opmode),
           (opmode & OP_MODE_DECODE_RGMII_MII_SEL) ? "MII" : "RGMII");

  int has_copper = (cfg_opmode == CFG_OPMODE_RGMII_COPPER ||
                    cfg_opmode == CFG_OPMODE_SGMII_COPPER ||
                    cfg_opmode == CFG_OPMODE_1000BT_1000BX ||
                    cfg_opmode == CFG_OPMODE_100BT_100FX);

  int has_fiber = (cfg_opmode == CFG_OPMODE_RGMII_1000BX ||
                   cfg_opmode == CFG_OPMODE_RGMII_100FX ||
                   cfg_opmode == CFG_OPMODE_1000BT_1000BX ||
                   cfg_opmode == CFG_OPMODE_100BT_100FX);

  if(has_copper) {
    uint16_t bmsr = reg_read(eni, REG_BMSR);
    bmsr = reg_read(eni, REG_BMSR); // Read twice -- link status is latched-low
    uint16_t phy_status = reg_read(eni, REG_PHY_STATUS);
    uint16_t rx_err_cnt = reg_read(eni, REG_RX_ERR_CNT);

    stprintf(s, "Copper: %s, %s %s duplex\n",
             (bmsr & BMSR_LINK_STS) ? "UP" : "DOWN",
             speed_str(phy_status),
             (phy_status & PHY_STATUS_DUPLEX) ? "full" : "half");

    if(rx_err_cnt)
      stprintf(s, "  RX errors: %d\n", rx_err_cnt);
  }

  if(has_fiber) {
    uint16_t fx_sts = reg_read(eni, REG_FX_STS);
    stprintf(s, "Fiber: %s, Auto-Neg %s\n",
             (fx_sts & (1 << 2)) ? "UP" : "DOWN",
             (fx_sts & (1 << 5)) ? "complete" : "pending");
  }

  if(!(opmode & OP_MODE_DECODE_RGMII_MII_SEL)) {
    uint16_t rgmii_ctrl = reg_read(eni, REG_RGMII_CTRL);
    stprintf(s, "RGMII delay: TX=%s RX=%s\n",
             (rgmii_ctrl & RGMII_CTRL_TX_CLK_DELAY) ? "off" : "on",
             (rgmii_ctrl & RGMII_CTRL_RX_CLK_DELAY) ? "off" : "on");
  }
}

static void dp83869_fiber_link_poll(struct ether_netif *eni)
  __attribute__((noreturn));

static void
dp83869_fiber_link_poll(struct ether_netif *eni)
{
  const char *name = eni->eni_ni.ni_dev.d_name;
  const ethmac_device_class_t *edc = (const void *)eni->eni_ni.ni_dev.d_class;
  int current_up = 0;

  while(1) {
    uint16_t fx_sts = reg_read(eni, REG_FX_STS);
    int up = !!(fx_sts & (1 << 2));  // Link status

    if(!current_up && up) {
      // 1000BASE-X is always 1000 Mbps full duplex
      evlog(LOG_INFO, "%s: Fiber link up 1000 Mbps full duplex", name);

      if(edc->edc_set_link_params)
        edc->edc_set_link_params(eni, 1000, 1);

      current_up = 1;
      net_task_raise(&eni->eni_ni.ni_task, NETIF_TASK_STATUS_UP);
    } else if(current_up && !up) {
      evlog(LOG_INFO, "%s: Fiber link down", name);
      current_up = 0;
      net_task_raise(&eni->eni_ni.ni_task, NETIF_TASK_STATUS_DOWN);
    }
    usleep(100000);
  }
}

static const ethphy_device_class_t dp83869_class = {
  .dc = {
    .dc_class_name = "DP83869 PHY",
    .dc_print_info = dp83869_print_diagnostics,
  },
};

static const ethphy_device_class_t dp83869_fiber_class = {
  .dc = {
    .dc_class_name = "DP83869 PHY",
    .dc_print_info = dp83869_print_diagnostics,
  },
  .link_poll = dp83869_fiber_link_poll,
};


static device_t *
dp83869_init(struct ether_netif *eni, ethphy_mode_t mode, unsigned int flags)
{
  uint16_t id2 = reg_read(eni, REG_PHYIDR2);
  uint16_t model = (id2 >> 4) & 0x3f;
  uint16_t rev = id2 & 0xf;

  uint16_t strap = reg_read(eni, REG_STRAP_STS);
  uint16_t opmode_strap = (strap >> 9) & 0x7;

  evlog(LOG_DEBUG, "dp83869: Model 0x%02x rev %d, strap opmode %d, "
        "phy_addr %d, aneg %s",
        model, rev, opmode_strap,
        (strap >> 4) & 0xf,
        (strap & 0x2) ? "enabled" : "disabled");

  if(mode != ETHPHY_MODE_RGMII && mode != ETHPHY_MODE_MII) {
    evlog(LOG_ERR, "dp83869: Unsupported MAC mode %d", mode);
    return NULL;
  }

  // Determine media from strap configuration:
  // opmode 0 = RGMII to Copper, 1 = RGMII to 1000Base-X, etc.
  dp83869_media_t media;
  switch(opmode_strap) {
  case 0: // RGMII to Copper
  case 6: // SGMII to Copper
    media = DP83869_MEDIA_COPPER;
    break;
  case 1: // RGMII to 1000Base-X
  case 2: // RGMII to 100Base-FX
  case 4: // 1000Base-T to 1000Base-X
  case 5: // 100Base-TX to 100Base-FX
    media = DP83869_MEDIA_FIBER;
    break;
  default:
    media = DP83869_MEDIA_COPPER;
    break;
  }

  if(media == DP83869_MEDIA_COPPER) {
    dp83869_set_mode_copper(eni, mode, flags);
    evlog(LOG_DEBUG, "dp83869: Configured for %sGMII-to-Copper",
          mode == ETHPHY_MODE_RGMII ? "R" : "");
  } else {
    dp83869_set_mode_fiber(eni, mode, flags);
    evlog(LOG_DEBUG, "dp83869: Configured for %sGMII-to-Fiber",
          mode == ETHPHY_MODE_RGMII ? "R" : "");

  }

  const ethphy_device_class_t *cls = (media == DP83869_MEDIA_FIBER) ?
    &dp83869_fiber_class : &dp83869_class;

  return ethphy_create((device_t *)eni, cls, sizeof(device_t));
}

static void *
dp83869_probe(driver_type_t type, device_t *parent)
{
  if(type != DRIVER_TYPE_ETHPHY)
    return NULL;

  struct ether_netif *eni = (struct ether_netif *)parent;

  uint16_t id1 = reg_read(eni, REG_PHYIDR1);
  uint16_t id2 = reg_read(eni, REG_PHYIDR2);

  // PHYIDR1 reset value = 0x2000, PHYIDR2 = 0xA0F1
  if(id1 != 0x2000 || (id2 & 0xfc00) != 0xa000)
    return NULL;

  uint16_t model = (id2 >> 4) & 0x3f;

  // DP83869HM model number = 0x0F
  if(model != 0x0f)
    return NULL;

  return &dp83869_init;
}

DRIVER(dp83869_probe, 5);
