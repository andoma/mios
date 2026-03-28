#include <mios/ethphy.h>
#include <mios/driver.h>
#include <mios/eventlog.h>

#include <net/ether.h>

#include <stdio.h>
#include <unistd.h>

// Standard MII registers
#define REG_BMCR     0x00
#define REG_BMSR     0x01
#define REG_PHYIDR1  0x02
#define REG_PHYIDR2  0x03
#define REG_ANAR     0x04
#define REG_GBCR     0x09  // 1000BASE-T Control
#define REG_PAGSEL   0x1f  // Page Select

// BMCR bits
#define BMCR_RESET        (1 << 15)
#define BMCR_AUTONEG_EN   (1 << 12)
#define BMCR_RESTART_ANEG (1 << 9)

// RTL8211F extended page 0xd08
#define RTL8211F_TX_DELAY  (1 << 8)  // Register 0x11
#define RTL8211F_RX_DELAY  (1 << 3)  // Register 0x15

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

static uint16_t
ext_read(struct ether_netif *eni, uint16_t page, uint16_t reg)
{
  reg_write(eni, REG_PAGSEL, page);
  uint16_t val = reg_read(eni, reg);
  reg_write(eni, REG_PAGSEL, 0);
  return val;
}

static void
ext_write(struct ether_netif *eni, uint16_t page, uint16_t reg, uint16_t val)
{
  reg_write(eni, REG_PAGSEL, page);
  reg_write(eni, reg, val);
  reg_write(eni, REG_PAGSEL, 0);
}

static void
rtl8211f_print_info(struct device *dev, struct stream *s)
{
  struct ether_netif *eni = (struct ether_netif *)dev->d_parent;
  stprintf(s, "RTL8211F Gigabit PHY\n");
  ethphy_print_status(eni, s);
}

static const device_class_t rtl8211f_class = {
  .dc_class_name = "RTL8211F PHY",
  .dc_print_info = rtl8211f_print_info,
};


static device_t *
rtl8211f_init(struct ether_netif *eni, ethphy_mode_t mode)
{
  uint16_t id2 = reg_read(eni, REG_PHYIDR2);

  evlog(LOG_INFO, "%s: RTL8211F rev %d",
        eni->eni_ni.ni_dev.d_name, id2 & 0xf);

  // Soft reset
  reg_write(eni, REG_BMCR, BMCR_RESET);
  for(int i = 0; i < 100; i++) {
    usleep(10000);
    if(!(reg_read(eni, REG_BMCR) & BMCR_RESET))
      break;
  }

  if(mode == ETHPHY_MODE_RGMII) {
    // Configure RGMII TX delay: enable (PHY adds 2ns on TX)
    uint16_t txdly = ext_read(eni, 0xd08, 0x11);
    txdly |= RTL8211F_TX_DELAY;
    ext_write(eni, 0xd08, 0x11, txdly);

    // Configure RGMII RX delay: enable (PHY adds 2ns on RX)
    uint16_t rxdly = ext_read(eni, 0xd08, 0x15);
    rxdly |= RTL8211F_RX_DELAY;
    ext_write(eni, 0xd08, 0x15, rxdly);
  }

  // Advertise 10/100/1000 full+half
  reg_write(eni, REG_ANAR, 0x01e1);  // 10/100 FD+HD + 802.3
  reg_write(eni, REG_GBCR, 0x0300);  // 1000 FD+HD

  // Enable and restart auto-negotiation
  reg_write(eni, REG_BMCR, BMCR_AUTONEG_EN | BMCR_RESTART_ANEG);

  return ethphy_create((device_t *)eni, &rtl8211f_class, sizeof(device_t));
}


static void *
rtl8211f_probe(driver_type_t type, device_t *parent)
{
  if(type != DRIVER_TYPE_ETHPHY)
    return NULL;

  struct ether_netif *eni = (struct ether_netif *)parent;

  uint16_t id1 = reg_read(eni, REG_PHYIDR1);
  uint16_t id2 = reg_read(eni, REG_PHYIDR2);

  // RTL8211F: PHYIDR1 = 0x001c, OUI = 0x00732, model = 0x11
  if(id1 != 0x001c)
    return NULL;

  uint16_t model = (id2 >> 4) & 0x3f;
  if(model != 0x11)
    return NULL;

  return &rtl8211f_init;
}

DRIVER(rtl8211f_probe, 5);
