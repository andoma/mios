#include "vi2c.h"

#include <string.h>
#include <sys/uio.h>
#include <mios/error.h>

/* A virtual I2C bus: a 256-byte register file for every 7-bit address.
   Devices are modelled entirely by the harness poking registers; the mios
   side sees an ordinary i2c_t. */
struct vi2c {
  i2c_t bus;                 /* must be first: bus == &v->bus */
  uint8_t regs[128][256];
};

/* In .data, not .bss: mios's init() clears .bss when the object boots, and
   a harness must be able to preset registers (chip ids, ready bits,
   calibration) before mios_sim_boot() so the drivers' probe sequences in
   main() find a plausible chip. */
static struct vi2c g_vi2c __attribute__((section(".data")));

/* Register-file semantics: an optional write of [reg, data...] then an
   optional read of N bytes from the (last) register pointer. */
static error_t
vi2c_rwv(i2c_t *bus, uint8_t addr, const struct iovec *tx,
         const struct iovec *rx, size_t count)
{
  struct vi2c *v = (struct vi2c *)bus;
  (void)count;
  if(addr >= 128)
    return ERR_NO_DEVICE;
  uint8_t *regs = v->regs[addr];

  uint8_t reg = 0;
  if(tx != NULL && tx->iov_len > 0) {
    const uint8_t *w = tx->iov_base;
    reg = w[0];
    for(size_t i = 1; i < tx->iov_len; i++)
      regs[(uint8_t)(reg + i - 1)] = w[i];
  }
  if(rx != NULL) {
    uint8_t *r = rx->iov_base;
    for(size_t i = 0; i < rx->iov_len; i++)
      r[i] = regs[(uint8_t)(reg + i)];
  }
  return 0;
}

static void __attribute__((constructor(200)))
vi2c_init(void)
{
  g_vi2c.bus.rwv = vi2c_rwv;
}

i2c_t *
vi2c_bus(void)
{
  return &g_vi2c.bus;
}

void
vi2c_set_reg(uint8_t addr, uint8_t reg, uint8_t val)
{
  if(addr < 128)
    g_vi2c.regs[addr][reg] = val;
}

uint8_t
vi2c_get_reg(uint8_t addr, uint8_t reg)
{
  return addr < 128 ? g_vi2c.regs[addr][reg] : 0xff;
}
