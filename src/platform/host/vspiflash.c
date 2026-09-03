#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdlib.h>
#include <sys/uio.h>

#include <mios/io.h>
#include <mios/mios.h>
#include <mios/error.h>

#include "vspiflash.h"

// Electronic id returned by 0xAB. Must be non-zero and non-0xff (the
// driver polls this to detect wake from deep-power-down).
#define VNOR_ID 0x17

typedef struct vnor {
  spi_t spi;
  uint8_t *mem;
  size_t size;
  uint8_t wel;              // write-enable latch
  uint8_t sfdp[0x60];

  // fault injection (see vspiflash.h)
  int fail_after;           // >=0: fail this program/erase op and later ones
  int corrupt_oneshot;      // flip a bit in the next programmed byte
} vnor_t;

// One CS-asserted transaction. rw()/rwv() start a fresh txn; the byte
// index carries across the (possibly several) iovec segments of one call.
typedef struct txn {
  int idx;
  uint8_t cmd;
  uint32_t addr;
} txn_t;


// Clock one byte through the chip, returning the byte shifted out on MISO.
static uint8_t
nor_byte(vnor_t *c, uint8_t in, txn_t *t)
{
  const int i = t->idx++;

  if(i == 0) {
    t->cmd = in;
    t->addr = 0;
    if(in == 0x06)         // WREN
      c->wel = 1;
    return 0xff;
  }

  switch(t->cmd) {
  case 0x05:               // RDSR: WIP=0 (instant), WEL in bit 1
    return c->wel << 1;

  case 0xab:               // release power-down / read electronic id
    return i >= 4 ? VNOR_ID : 0x00;

  case 0x5a:               // read SFDP: cmd, 3 addr bytes, 1 dummy, data
    if(i <= 3) { t->addr = (t->addr << 8) | in; return 0x00; }
    if(i == 4) return 0x00;
    {
      uint32_t a = t->addr + (i - 5);
      return a < sizeof(c->sfdp) ? c->sfdp[a] : 0x00;
    }

  case 0x03:               // read data: cmd, 3 addr bytes, data
    if(i <= 3) { t->addr = (t->addr << 8) | in; return 0xff; }
    {
      uint32_t a = t->addr + (i - 4);
      return a < c->size ? c->mem[a] : 0xff;
    }

  case 0x02:               // page program: cmd, 3 addr bytes, data
    if(i <= 3) { t->addr = (t->addr << 8) | in; return 0xff; }
    if(c->wel) {
      uint32_t a = t->addr + (i - 4);
      if(a < c->size) {
        uint8_t v = in;
        if(c->corrupt_oneshot) {   // simulate a bad cell
          v ^= 0x01;
          c->corrupt_oneshot = 0;
        }
        c->mem[a] &= v;   // NOR programming only clears bits
      }
    }
    return 0xff;

  case 0x20:               // 4 kB sector erase: cmd, 3 addr bytes
    if(i <= 3) {
      t->addr = (t->addr << 8) | in;
      if(i == 3 && c->wel) {
        uint32_t base = t->addr & ~0xfffu;
        if(base < c->size)
          memset(c->mem + base, 0xff, 0x1000);
      }
    }
    return 0xff;

  default:                 // 0xb9 (deep power down) and anything else: nop
    return 0xff;
  }
}


static void
nor_run(vnor_t *c, const uint8_t *tx, uint8_t *rx, size_t len, txn_t *t)
{
  for(size_t k = 0; k < len; k++) {
    uint8_t out = nor_byte(c, tx ? tx[k] : 0, t);
    if(rx)
      rx[k] = out;
  }
}


// The whole rwv() call is one CS assertion, so one txn spans all segments.
// Length transferred per segment is txiov[i].iov_len (rxiov len is ignored,
// matching the real SPI drivers).
static error_t
vspi_rwv(struct spi *bus, const struct iovec *txiov, const struct iovec *rxiov,
         size_t count, gpio_t nss, int config)
{
  vnor_t *c = (vnor_t *)bus;

  const uint8_t cmd = (count && txiov[0].iov_base && txiov[0].iov_len)
    ? ((const uint8_t *)txiov[0].iov_base)[0] : 0;
  if((cmd == 0x02 || cmd == 0x20) && c->fail_after >= 0) {
    if(c->fail_after == 0) {
      c->wel = 0;
      return ERR_FLASH_TIMEOUT;   // flash reports an I/O error
    }
    c->fail_after--;
  }

  txn_t t = {0};
  for(size_t i = 0; i < count; i++)
    nor_run(c, txiov[i].iov_base, rxiov ? rxiov[i].iov_base : NULL,
            txiov[i].iov_len, &t);

  if(t.cmd == 0x02 || t.cmd == 0x20)
    c->wel = 0;            // program/erase consume the write-enable latch
  return 0;
}

static error_t
vspi_rw(struct spi *bus, const uint8_t *tx, uint8_t *rx, size_t len,
        gpio_t nss, int config)
{
  struct iovec txv = { (void *)tx, len };
  struct iovec rxv = { rx, len };
  return vspi_rwv(bus, &txv, &rxv, 1, nss, config);
}

static error_t
vspi_rw_locked(struct spi *bus, const uint8_t *tx, uint8_t *rx, size_t len,
               gpio_t nss, int config)
{
  return vspi_rw(bus, tx, rx, len, nss, config);
}

static void
vspi_lock(struct spi *bus, int acquire)
{
}

static int
vspi_get_config(struct spi *bus, int clock_flags, int baudrate)
{
  return 0;
}


static void
sfdp_wr(uint8_t *s, uint32_t off, uint32_t v)
{
  s[off + 0] = v;
  s[off + 1] = v >> 8;
  s[off + 2] = v >> 16;
  s[off + 3] = v >> 24;
}


struct spi *
vspiflash_create(size_t size)
{
  vnor_t *c = calloc(1, sizeof(vnor_t));
  c->size = size;
  c->mem = malloc(size);
  memset(c->mem, 0xff, size);
  c->fail_after = -1;

  // Minimal but valid SFDP: header + one JEDEC parameter table at 0x30
  // advertising the density and a single 4 kB (0x20) sector-erase command.
  const uint32_t density = (uint32_t)(size << 3) - 1;
  sfdp_wr(c->sfdp, 0x00, 0x50444653);   // "SFDP"
  sfdp_wr(c->sfdp, 0x04, 0xff000106);   // nph (bits 16..23) = 0 -> 1 header
  sfdp_wr(c->sfdp, 0x08, 0xff000000);   // param header 0: id byte = 0 (JEDEC)
  sfdp_wr(c->sfdp, 0x0c, 0x00000030);   // param table pointer -> 0x30
  sfdp_wr(c->sfdp, 0x34, density);      // ptp+0x04: flash density (bits - 1)
  sfdp_wr(c->sfdp, 0x4c, 0x0000200c);   // ptp+0x1c: erase[0] = 2^12, cmd 0x20
  sfdp_wr(c->sfdp, 0x50, 0x00000000);   // ptp+0x20: erase[2],[3] unused
  sfdp_wr(c->sfdp, 0x54, 0x00000000);   // ptp+0x24: erase timings (-> ~1 ms)

  c->spi.rw = vspi_rw;
  c->spi.rwv = vspi_rwv;
  c->spi.rw_locked = vspi_rw_locked;
  c->spi.lock = vspi_lock;
  c->spi.get_config = vspi_get_config;
  return &c->spi;
}


// ---- Fault injection ----

void
vspiflash_fail_after(struct spi *bus, int ops)
{
  ((vnor_t *)bus)->fail_after = ops;
}

void
vspiflash_corrupt_next_write(struct spi *bus)
{
  ((vnor_t *)bus)->corrupt_oneshot = 1;
}

void
vspiflash_poke(struct spi *bus, uint32_t addr)
{
  vnor_t *c = (vnor_t *)bus;
  // Rot one bit from 1 to 0. Skip forward to a byte that actually has a set
  // bit so the corruption is guaranteed to change the stored data.
  for(uint32_t a = addr; a < c->size; a++) {
    if(c->mem[a] != 0x00) {
      c->mem[a] &= c->mem[a] - 1;   // clear the lowest set bit
      return;
    }
  }
}
