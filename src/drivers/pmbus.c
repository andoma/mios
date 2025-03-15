#include <mios/pmbus.h>

#include <mios/io.h>

#include <stdlib.h>
#include <math.h>
#include <unistd.h>

#include "util/crc8.h"

struct pmbus {
  i2c_t *bus;
  uint8_t addr;

  // Some devices needs to "rest" between i2c transactions
  // This specifices the interval in µs
  int com_interval;

  uint64_t next_com;
};

float
pmbus_decode_linear11(int16_t i16)
{
  if((uint16_t)i16 == 0xffff)
    return NAN;

  int16_t exp = i16 >> 11;
  uint16_t m = i16 & ((1 << 11) - 1);
  return (float)m * powf(2, exp);
}


static int
check_read_crc(pmbus_t *p, uint8_t reg, const uint8_t *rdbuf, size_t rdbuf_size)
{
  const uint8_t crc = crc8(0, (const uint8_t[]){
      (p->addr << 1), reg, (p->addr << 1) | 1}, 3);
  return crc8(crc, rdbuf, rdbuf_size);
}


static error_t
pmbus_i2c(pmbus_t *p, const uint8_t *wrbuf, size_t wrlen,
          uint8_t *rdbuf, size_t rdlen)
{
  if(p->next_com)
    sleep_until(p->next_com);

  error_t err = i2c_rw(p->bus, p->addr, wrbuf, wrlen, rdbuf, rdlen);

  if(p->com_interval)
    p->next_com = clock_get() + p->com_interval;

  return err;
}


int
pmbus_read_8(pmbus_t *p, uint8_t reg)
{
  uint8_t wrbuf[1] = {reg};
  uint8_t rdbuf[2] = {};

  error_t err = pmbus_i2c(p, wrbuf, sizeof(wrbuf), rdbuf, sizeof(rdbuf));
  if(err)
    return err;

  if(check_read_crc(p, reg, rdbuf, sizeof(rdbuf)))
    return ERR_CHECKSUM_ERROR;
  return rdbuf[0];
}


int
pmbus_read_16(pmbus_t *p, uint8_t reg)
{
  uint8_t wrbuf[1] = {reg};
  uint8_t rdbuf[3] = {};

  error_t err = pmbus_i2c(p, wrbuf, sizeof(wrbuf), rdbuf, sizeof(rdbuf));
  if(err)
    return err;

  if(check_read_crc(p, reg, rdbuf, sizeof(rdbuf)))
    return ERR_CHECKSUM_ERROR;

  return rdbuf[0] | (rdbuf[1] << 8);
}


error_t
pmbus_write_8(pmbus_t *p, uint8_t reg, uint8_t value)
{
  uint8_t wrbuf[3] = {reg, value};

  wrbuf[2] = crc8(crc8(0, (const uint8_t[]){p->addr << 1}, 1),
                  wrbuf, 2);

  return pmbus_i2c(p, wrbuf, sizeof(wrbuf), NULL, 0);
}


pmbus_t *
pmbus_create(struct i2c *bus, uint8_t address, int com_interval_us)
{
  pmbus_t *p = calloc(1, sizeof(pmbus_t));
  p->bus = bus;
  p->addr = address;
  p->com_interval = com_interval_us;
  return p;
}
