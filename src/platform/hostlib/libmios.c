#include "libmios.h"
#include "hostlib.h"
#include "vcan.h"

#include <unistd.h>          /* clock_get */

void host_lib_boot(void);
void host_lib_step(uint64_t dt_us);

#define EXPORT __attribute__((visibility("default")))

EXPORT void     mios_sim_boot(void)            { host_lib_boot(); }
EXPORT void     mios_sim_step(uint64_t dt)     { host_lib_step(dt); }
EXPORT uint64_t mios_sim_time(void)            { return clock_get(); }

EXPORT void
mios_sim_i2c_set(uint8_t addr, uint8_t reg, uint8_t val)
{
  vi2c_set_reg(addr, reg, val);
}

EXPORT uint8_t
mios_sim_i2c_get(uint8_t addr, uint8_t reg)
{
  return vi2c_get_reg(addr, reg);
}

EXPORT void
mios_sim_spi_set(uint8_t dev, uint8_t reg, uint8_t val)
{
  vspi_set_reg(dev, reg, val);
}

EXPORT uint8_t
mios_sim_spi_get(uint8_t dev, uint8_t reg)
{
  return vspi_get_reg(dev, reg);
}


/* The one virtual CAN interface. Created from the app's main() (mios
   context: allocates, attaches a netif); the ABI side only touches its
   rings, which is all a harness may do from outside. */
static vcan_t *g_vcan;

vcan_t *
hostlib_can(void)
{
  if(g_vcan == NULL) {
    g_vcan = vcan_create("vcan0", 64);
    vcan_set_link(g_vcan, 1);
  }
  return g_vcan;
}

EXPORT ssize_t
mios_sim_can_recv(uint32_t *id, void *buf, size_t buflen)
{
  if(g_vcan == NULL)
    return -1;
  return vcan_peer_poll(g_vcan, id, buf, buflen);
}

EXPORT int
mios_sim_can_send(uint32_t id, const void *payload, size_t len)
{
  if(g_vcan == NULL)
    return -1;
  vcan_peer_send(g_vcan, id, payload, len);
  return 0;
}
