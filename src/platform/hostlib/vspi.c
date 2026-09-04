#include "vspi.h"

#include <string.h>
#include <sys/uio.h>
#include <mios/error.h>

struct vspi {
  spi_t bus;                 /* must be first: bus == &v->bus */
  uint8_t regs[VSPI_NDEV][256];
};

/* In .data, not .bss: mios's init() clears .bss when the object boots, and
   a harness must be able to preset registers (chip ids, ready bits,
   calibration) before mios_sim_boot() so the drivers' probe sequences in
   main() find a plausible chip. */
static struct vspi g_vspi __attribute__((section(".data")));

/* One chip-select assertion: the first byte selects register and
   direction, the rest of the transfer streams consecutive registers. tx
   and rx may alias (drivers commonly reuse one scratch buffer), so the
   command byte is consumed before anything is written back. */
static error_t
vspi_rwv(spi_t *bus, const struct iovec *txiov, const struct iovec *rxiov,
         size_t count, gpio_t nss, int config)
{
  struct vspi *v = (struct vspi *)bus;
  (void)config;
  if(nss < 0 || nss >= VSPI_NDEV)
    return ERR_NO_DEVICE;
  uint8_t *regs = v->regs[nss];

  int have_cmd = 0;
  int read = 0;
  uint8_t reg = 0;
  for(size_t i = 0; i < count; i++) {
    const uint8_t *tx = txiov ? txiov[i].iov_base : NULL;
    uint8_t *rx = rxiov ? rxiov[i].iov_base : NULL;
    const size_t len = txiov ? txiov[i].iov_len : rxiov[i].iov_len;
    for(size_t j = 0; j < len; j++) {
      const uint8_t t = tx ? tx[j] : 0;
      uint8_t r = 0;
      if(!have_cmd) {
        have_cmd = 1;
        read = t & 0x80;
        reg = t & 0x7f;
      } else if(read) {
        r = regs[reg++];
      } else {
        regs[reg++] = t;
      }
      if(rx)
        rx[j] = r;
    }
  }
  return 0;
}

static error_t
vspi_rw(spi_t *bus, const uint8_t *tx, uint8_t *rx, size_t len,
        gpio_t nss, int config)
{
  struct iovec t = { .iov_base = (void *)tx, .iov_len = len };
  struct iovec r = { .iov_base = rx, .iov_len = len };
  return vspi_rwv(bus, tx ? &t : NULL, rx ? &r : NULL, 1, nss, config);
}

static void
vspi_lock(spi_t *bus, int acquire)
{
}

static int
vspi_get_config(spi_t *bus, int clock_flags, int baudrate)
{
  return 0;
}

static void __attribute__((constructor(200)))
vspi_init(void)
{
  g_vspi.bus.rw = vspi_rw;
  g_vspi.bus.rwv = vspi_rwv;
  g_vspi.bus.rw_locked = vspi_rw;
  g_vspi.bus.lock = vspi_lock;
  g_vspi.bus.get_config = vspi_get_config;
}

spi_t *
vspi_bus(void)
{
  return &g_vspi.bus;
}

void
vspi_set_reg(uint8_t dev, uint8_t reg, uint8_t val)
{
  if(dev < VSPI_NDEV)
    g_vspi.regs[dev][reg] = val;
}

uint8_t
vspi_get_reg(uint8_t dev, uint8_t reg)
{
  return dev < VSPI_NDEV ? g_vspi.regs[dev][reg] : 0xff;
}
