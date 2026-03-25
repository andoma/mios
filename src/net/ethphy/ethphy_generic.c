#include <mios/ethphy.h>
#include <mios/driver.h>
#include <mios/eventlog.h>

#include <net/ether.h>

static const device_class_t ethphy_generic = {
  .dc_class_name = "Generic PHY",
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
