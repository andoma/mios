#include "tps92682.h"

#include <stdlib.h>
#include <malloc.h>
#include <string.h>

// https://www.ti.com/lit/ds/symlink/tps92682-q1.pdf

#define TPS92682_EN   0x00
#define TPS92682_CFG1 0x01
#define TPS92682_CFG2 0x02
#define TPS92682_FM   0x05

#define TPS92682_CH1ADJ 0x07
#define TPS92682_CH2ADJ 0x08

#define TPS92682_CHxADJ(x) (0x07 + (x))

#define TPS92682_CH1PWML 0x0a
#define TPS92682_CH1PWMH 0x0b
#define TPS92682_CH2PWML 0x0c
#define TPS92682_CH2PWMH 0x0d

#define TPS92682_CHxPWML(x) (0x0a + (x) * 2)
#define TPS92682_CHxPWMH(x) (0x0b + (x) * 2)

#define TPS92682_FLT1  0x11
#define TPS92682_FLT2  0x12
#define TPS92682_FEN1  0x13
#define TPS92682_FEN2  0x14

#define TPS92682_RESET 0x26

typedef struct tps92682 {
  spi_t *spi;
  gpio_t cs;
  uint32_t spi_config;
  uint8_t tx[2];
  uint8_t rx[2];
} tps92682_t;


static error_t
write_reg(tps92682_t *t, uint8_t reg, uint8_t value)
{
  t->tx[0] = 0x80 | reg << 1;
  t->tx[1] = value;
  t->tx[0] |= !(__builtin_popcount(t->tx[0] ^ t->tx[1]) & 1);
  return t->spi->rw(t->spi, t->tx, NULL, 2, t->cs, t->spi_config);
}


int
tps92682_read_reg(tps92682_t *t, uint8_t reg)
{
  error_t err;
  t->tx[0] = reg << 1;
  t->tx[1] = 0;
  t->tx[0] |= !(__builtin_popcount(t->tx[0] ^ t->tx[1]) & 1);
  err = t->spi->rw(t->spi, t->tx, NULL, 2, t->cs, t->spi_config);
  if(err)
    return err;
  err = t->spi->rw(t->spi, t->tx, t->rx, 2, t->cs, t->spi_config);
  if(err)
    return err;
  return t->rx[1];
}


static void
tps92682_set_duty(tps92682_t *t, int channel, unsigned int duty)
{
  duty &= 0x3ff;
  write_reg(t, TPS92682_CHxPWMH(channel), duty >> 8);
  write_reg(t, TPS92682_CHxPWML(channel), duty & 0xff);
}


error_t
tps92682_init(tps92682_t *t)
{
  write_reg(t, TPS92682_RESET, 0xc3); // Magic value for reset

  int val = tps92682_read_reg(t, TPS92682_EN);
  if(val < 0)
    return val;

  if(val != 0x3c)
    return ERR_NO_DEVICE;

  write_reg(t, TPS92682_CFG1, 0x40);  // Internal PWM

  tps92682_read_reg(t, TPS92682_FLT1); // Read to ACK faults

  write_reg(t, TPS92682_CH1ADJ, 0x00); // Max current limit to zer0
  write_reg(t, TPS92682_CH2ADJ, 0x00); // Max current limit to zer0

  tps92682_set_duty(t, 0, 0x3ff);
  tps92682_set_duty(t, 1, 0x3ff);

  write_reg(t, TPS92682_EN, 0xbf);
  return 0;
}


struct tps92682 *
tps92682_create(spi_t *spi, gpio_t cs)
{
  tps92682_t *t = xalloc(sizeof(tps92682_t), 0, MEM_TYPE_DMA);
  memset(t, 0, sizeof(tps92682_t));

  t->spi = spi;
  t->cs = cs;
  t->spi_config = spi->get_config(spi, 0, 1000000);
  return t;
}


error_t
tps92682_set_ilimit_raw(struct tps92682 *t, unsigned int channel,
                        unsigned int value)
{
  write_reg(t, TPS92682_CHxADJ(channel), value);
  return 0;
}


error_t
tps92682_set_spread_specturm(struct tps92682 *t, int magnitude)
{
  uint8_t val = (magnitude << 4) | 0b0101;
  return write_reg(t, TPS92682_FM, val);
}
