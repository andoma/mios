#include <mios/ethphy.h>
#include "ether.h"
#include "netif.h"

#include <stdlib.h>
#include <unistd.h>

#include <mios/eventlog.h>

#define MII_BMSR    0x01
#define MII_ANAR    0x04
#define MII_ANLPAR  0x05
#define MII_GBCR    0x09  // 1000BASE-T Control
#define MII_GBSR    0x0a  // 1000BASE-T Status
#define REG_REGCR   0x0d
#define REG_ADDAR   0x0e

#define BMSR_LINK_STATUS   (1 << 2)
#define BMSR_EXTENDED_STATUS (1 << 8)

#define ANLPAR_100TX_FD   (1 << 8)
#define ANLPAR_100TX_HD   (1 << 7)
#define ANLPAR_10T_FD     (1 << 6)

#define GBCR_1000T_FD     (1 << 9)
#define GBCR_1000T_HD     (1 << 8)
#define GBSR_LP_1000T_FD  (1 << 11)
#define GBSR_LP_1000T_HD  (1 << 10)


static int
mii_read0(ether_netif_t *eni, uint16_t reg, const ethmac_device_class_t *edc)
{
  return edc->edc_mii_read(eni, reg);
}

error_t
mii_write0(ether_netif_t *eni, uint16_t reg, uint16_t value,
           const ethmac_device_class_t *edc)
{
  return edc->edc_mii_write(eni, reg, value);
}

int
ethphy_mii_read(ether_netif_t *eni, uint16_t reg)
{
  const ethmac_device_class_t *edc = (const void *)eni->eni_ni.ni_dev.d_class;

  mutex_lock(&eni->eni_phy_ext_mutex);

  int r;
  if(reg < 0x20) {
    r = mii_read0(eni, reg, edc);
  } else {
    r = mii_write0(eni, REG_REGCR, 0x1f, edc);
    if(!r)
      r = mii_write0(eni, REG_ADDAR, reg, edc);
    if(!r)
      r = mii_write0(eni, REG_REGCR, 0x401f, edc);
    if(!r)
      r = mii_read0(eni, REG_ADDAR, edc);
  }

  mutex_unlock(&eni->eni_phy_ext_mutex);
  return r;
}

error_t
ethphy_mii_write(ether_netif_t *eni, uint16_t reg, uint16_t val)
{
  const ethmac_device_class_t *edc = (const void *)eni->eni_ni.ni_dev.d_class;

  mutex_lock(&eni->eni_phy_ext_mutex);

  error_t err;
  if(reg < 0x20) {
    err = mii_write0(eni, reg, val, edc);
  } else {
    err = mii_write0(eni, REG_REGCR, 0x1f, edc);
    if(!err)
      err = mii_write0(eni, REG_ADDAR, reg, edc);
    if(!err)
      err = mii_write0(eni, REG_REGCR, 0x401f, edc);
    if(!err)
      err = mii_write0(eni, REG_ADDAR, val, edc);
  }

  mutex_unlock(&eni->eni_phy_ext_mutex);
  return err;
}




void
ethphy_link_poll(ether_netif_t *eni)
{
  // Check if PHY provides a custom link poll
  if(eni->eni_phy != NULL) {
    const ethphy_device_class_t *pdc =
      (const ethphy_device_class_t *)eni->eni_phy->d_class;
    if(pdc->link_poll != NULL)
      pdc->link_poll(eni);
  }

  const char *name = eni->eni_ni.ni_dev.d_name;
  const ethmac_device_class_t *edc = (const void *)eni->eni_ni.ni_dev.d_class;
  int current_up = 0;

  while(1) {
    ethphy_mii_read(eni, MII_BMSR);
    int n = ethphy_mii_read(eni, MII_BMSR);
    int up = !!(n & BMSR_LINK_STATUS);

    if(!current_up && up) {

      int speed = 10;
      int full_duplex = 0;

      int gbcr = (n & BMSR_EXTENDED_STATUS) ?
        ethphy_mii_read(eni, MII_GBCR) : 0;
      int gbsr = (n & BMSR_EXTENDED_STATUS) ?
        ethphy_mii_read(eni, MII_GBSR) : 0;

      if((gbcr & GBCR_1000T_FD) && (gbsr & GBSR_LP_1000T_FD)) {
        speed = 1000;
        full_duplex = 1;
      } else if((gbcr & GBCR_1000T_HD) && (gbsr & GBSR_LP_1000T_HD)) {
        speed = 1000;
      } else {
        int adv = ethphy_mii_read(eni, MII_ANAR);
        int lpa = ethphy_mii_read(eni, MII_ANLPAR);
        int common = adv & lpa;

        if(common & ANLPAR_100TX_FD) {
          speed = 100;
          full_duplex = 1;
        } else if(common & ANLPAR_100TX_HD) {
          speed = 100;
        } else if(common & ANLPAR_10T_FD) {
          full_duplex = 1;
        }
      }

      evlog(LOG_INFO, "%s: Link up %d Mbps %s duplex",
            name, speed, full_duplex ? "full" : "half");

      if(edc->edc_set_link_params)
        edc->edc_set_link_params(eni, speed, full_duplex);

      current_up = 1;
      net_task_raise(&eni->eni_ni.ni_task, NETIF_TASK_STATUS_UP);
    } else if(current_up && !up) {
      evlog(LOG_INFO, "%s: Link down", name);
      current_up = 0;
      net_task_raise(&eni->eni_ni.ni_task, NETIF_TASK_STATUS_DOWN);
    }
    usleep(100000);
  }
}


device_t *
ethphy_create(device_t *parent, const ethphy_device_class_t *dc, size_t size)
{
  device_t *d = calloc(1, size);
  d->d_name = parent->d_name;
  d->d_class = &dc->dc;
  d->d_parent = parent;
  device_retain(parent);

  device_register(d);

  return d;
}
