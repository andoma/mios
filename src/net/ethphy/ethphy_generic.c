#include <mios/ethphy.h>
#include <mios/driver.h>
#include <mios/eventlog.h>
#include <stdio.h>

#include <net/ether.h>

void
ethphy_print_status(struct ether_netif *eni, struct stream *s)
{
  uint16_t bmsr = ethphy_mii_read(eni, 0x01);
  bmsr = ethphy_mii_read(eni, 0x01); // Read twice -- link status is latched-low

  if(bmsr & (1 << 2)) {
    int speed = 10;
    int full_duplex = 0;

    if(bmsr & (1 << 8)) {
      uint16_t gbcr = ethphy_mii_read(eni, 0x09);
      uint16_t gbsr = ethphy_mii_read(eni, 0x0a);
      if((gbcr & (1 << 9)) && (gbsr & (1 << 11))) {
        speed = 1000; full_duplex = 1;
      } else if((gbcr & (1 << 8)) && (gbsr & (1 << 10))) {
        speed = 1000;
      }
    }

    if(speed < 1000) {
      uint16_t adv = ethphy_mii_read(eni, 0x04);
      uint16_t lpa = ethphy_mii_read(eni, 0x05);
      int common = adv & lpa;
      if(common & (1 << 8)) {
        speed = 100; full_duplex = 1;
      } else if(common & (1 << 7)) {
        speed = 100;
      } else if(common & (1 << 6)) {
        full_duplex = 1;
      }
    }

    stprintf(s, "Link: UP, %dM %s duplex\n",
             speed, full_duplex ? "full" : "half");
  } else {
    stprintf(s, "Link: DOWN\n");
  }
}

static void
generic_print_info(struct device *dev, struct stream *s)
{
  struct ether_netif *eni = (struct ether_netif *)dev->d_parent;
  uint16_t id1 = ethphy_mii_read(eni, 0x02);
  uint16_t id2 = ethphy_mii_read(eni, 0x03);

  stprintf(s, "PHY OUI:0x%05x Model:0x%02x Rev:%d\n",
           (id1 << 6) | (id2 >> 10), (id2 >> 4) & 0x3f, id2 & 0xf);
  ethphy_print_status(eni, s);
}

static const device_class_t ethphy_generic = {
  .dc_class_name = "Generic PHY",
  .dc_print_info = generic_print_info,
};

static device_t *
generic_init(struct ether_netif *eni, ethphy_mode_t mode)
{
  const uint16_t id1 = ethphy_mii_read(eni, 0x02);
  const uint16_t id2 = ethphy_mii_read(eni, 0x03);

  evlog(LOG_INFO, "%s: PHY OUI:0x%05x Model:0x%02x Rev:%d",
        eni->eni_ni.ni_dev.d_name,
        (id1 << 6) | (id2 >> 10), (id2 >> 4) & 0x3f, id2 & 0xf);

  return ethphy_create((device_t *)eni, &ethphy_generic, sizeof(device_t));
}

static void *
generic_probe(driver_type_t type, device_t *parent)
{
  if(type != DRIVER_TYPE_ETHPHY)
    return NULL;
  return &generic_init;
}

DRIVER(generic_probe, 9);
