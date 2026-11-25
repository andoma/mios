#include "libmios.h"
#include "hostlib.h"
#include "vcan.h"
#include "vregfile.h"

#include <unistd.h>          /* clock_get */

void host_lib_boot(void);
void host_lib_step(uint64_t dt_us);

#define EXPORT __attribute__((visibility("default")))

EXPORT void     mios_sim_boot(void)            { host_lib_boot(); }
EXPORT void     mios_sim_step(uint64_t dt)     { host_lib_step(dt); }
EXPORT uint64_t mios_sim_time(void)            { return clock_get(); }

/* The register files the harness reads and writes. In .data, not .bss:
   mios's init() clears .bss when the object boots, and a harness must be
   able to preset registers (chip ids, ready bits, calibration) before
   mios_sim_boot() so the drivers' probe sequences in main() find a
   plausible chip. */
static vregfile_t g_i2c_files[128] __attribute__((section(".data")));
static vregfile_t g_spi_files[HOSTLIB_SPI_NDEV] __attribute__((section(".data")));

static i2c_t *g_i2c;
static spi_t *g_spi;

i2c_t *
hostlib_i2c(void)
{
  if(g_i2c == NULL) {
    g_i2c = vi2c_create("vi2c");
    for(int a = 0; a < 128; a++)
      vregfile_attach_i2c(&g_i2c_files[a], g_i2c, a);
  }
  return g_i2c;
}

spi_t *
hostlib_spi(void)
{
  if(g_spi == NULL) {
    g_spi = vspi_create("vspi");
    for(int cs = 0; cs < HOSTLIB_SPI_NDEV; cs++) {
      gpio_set_output(cs, 1);
      gpio_conf_output(cs, GPIO_PUSH_PULL, GPIO_SPEED_HIGH, GPIO_PULL_NONE);
      vregfile_attach_spi(&g_spi_files[cs], g_spi, cs);
    }
  }
  return g_spi;
}

EXPORT void
mios_sim_i2c_set(uint8_t addr, uint8_t reg, uint8_t val)
{
  if(addr < 128)
    g_i2c_files[addr].regs[reg] = val;
}

EXPORT uint8_t
mios_sim_i2c_get(uint8_t addr, uint8_t reg)
{
  return addr < 128 ? g_i2c_files[addr].regs[reg] : 0xff;
}

EXPORT void
mios_sim_spi_set(uint8_t dev, uint8_t reg, uint8_t val)
{
  if(dev < HOSTLIB_SPI_NDEV)
    g_spi_files[dev].regs[reg] = val;
}

EXPORT uint8_t
mios_sim_spi_get(uint8_t dev, uint8_t reg)
{
  return dev < HOSTLIB_SPI_NDEV ? g_spi_files[dev].regs[reg] : 0xff;
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
