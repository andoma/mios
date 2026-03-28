#include <mios/ethphy.h>
#include <mios/driver.h>
#include <mios/eventlog.h>
#include <stdio.h>

// Datasheet: https://www.ti.com/lit/ds/symlink/dp83826e.pdf

#define REG_BMSR    0x0001
#define REG_PHYIDR1 0x0002
#define REG_PHYIDR2 0x0003
#define REG_PHYSTS  0x0010
#define REG_RCSR    0x0017
#define REG_SOR1    0x0467


device_t *dp83826_init(struct ether_netif *eni, ethphy_mode_t mode, unsigned int flags);



static void
dp83826_print_info(struct device *dev, struct stream *s)
{
  struct ether_netif *eni = (struct ether_netif *)dev->d_parent;

  uint16_t id2 = ethphy_mii_read(eni, REG_PHYIDR2);
  uint16_t model = (id2 >> 4) & 0x3f;

  uint16_t rcsr = ethphy_mii_read(eni, REG_RCSR);
  stprintf(s, "DP83826%c rev %d  Interface: %sMII\n",
           model == 0x13 ? 'E' : 'I',
           id2 & 0xf,
           (rcsr & 0x20) ? "R" : "");
  ethphy_print_status(eni, s);
}

static const ethphy_device_class_t ethphy_dp83826 = {
  .dc = {
    .dc_class_name = "DP83826 PHY",
    .dc_print_info = dp83826_print_info,
  },
};


device_t *
dp83826_init(struct ether_netif *eni, ethphy_mode_t mode, unsigned int flags)
{
  const uint16_t sor = ethphy_mii_read(eni, REG_SOR1);
  const uint16_t id2 = ethphy_mii_read(eni, REG_PHYIDR2);
  const uint16_t model = (id2 >> 4) & 0x3f;

  char strapbits[12];
  for(int i = 0; i < 11; i++) {
    strapbits[i] = (sor >> i) & 1 ? '1' : '0';
  }
  strapbits[11] = 0;

  evlog(LOG_DEBUG, "dp83826: Revision %d in %s mode (Strap:%s)",
        id2 & 0xf, model == 0x13 ? "Enhanced" : "Basic", strapbits);

  uint16_t rcsr = ethphy_mii_read(eni, REG_RCSR);
  if(mode == ETHPHY_MODE_RMII) {
    rcsr |= 0x20;
  } else {
    rcsr &= ~0x20;
  }
  evlog(LOG_DEBUG, "dp83826: Running in %sMII mode",
        mode == ETHPHY_MODE_RMII ? "R":"");

  ethphy_mii_write(eni, REG_RCSR, rcsr);

  return ethphy_create((device_t *)eni, &ethphy_dp83826, sizeof(device_t));
}

static void *
dp83826_probe(driver_type_t type, device_t *parent)
{
  if(type != DRIVER_TYPE_ETHPHY)
    return NULL;

  // For DRIVER_TYPE_ETHPHY, the parent is a ether_netif_t device
  struct ether_netif *eni = (struct ether_netif *)parent;

  const uint16_t id1 = ethphy_mii_read(eni, REG_PHYIDR1);
  const uint16_t id2 = ethphy_mii_read(eni, REG_PHYIDR2);

  if(id1 != 0x2000 || (id2 & 0xfc00) != 0xa000) {
    return NULL;
  }
  const uint16_t model = (id2 >> 4) & 0x3f;
  if(model != 0x13 && model != 0x11) {
    return NULL;
  }

  return &dp83826_init;
}

DRIVER(dp83826_probe, 5);
