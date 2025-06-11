#include "dac8563.h"

#include <malloc.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <mios/eventlog.h>

enum  DAC_CMDS {
  REG_INPUT_A_UPD = 0,
  REG_INPUT_B_UPD,
  REG_PWR_UP_A,
  REG_PWR_UP_B,
  REG_PWR_DWN_HIZ_A,
  REG_PWR_DWN_HIZ_B,
  REG_RST,
  REG_NO_LDAC,
  REG_INT_REF,
};


static const uint8_t dac_cmds[][3] = {
  [REG_INPUT_A_UPD] =   { 0b00011000, 0, 0},
  [REG_INPUT_B_UPD] =   { 0b00011001, 0, 0},
  [REG_PWR_UP_A] =      { 0b00100000, 0, 0b00000001},
  [REG_PWR_UP_B] =      { 0b00100000, 0, 0b00000010},
  [REG_PWR_DWN_HIZ_A] = { 0b00100000, 0, 0b00011001},
  [REG_PWR_DWN_HIZ_B] = { 0b00100000, 0, 0b00011010},
  [REG_RST] =           { 0b00100000, 0, 0b00011011},
  [REG_NO_LDAC] =       { 0b00110000, 0, 0b00000011},
  [REG_INT_REF] =       { 0b00111000, 0, 0b00000001},
};

struct dac8563 {
  spi_t *spi;
  gpio_t nss;
  int spicfg;
};

static error_t
write_cmd(dac8563_t *dev, uint8_t cmd, uint16_t *value)
{
  if (cmd > REG_INT_REF)
    return ERR_INVALID_PARAMETER;

  if (!value)
    return dev->spi->rw(dev->spi, dac_cmds[cmd], NULL, 3, dev->nss, dev->spicfg);

  uint8_t tx[3];
  memcpy(tx, &dac_cmds[cmd], 3);
  tx[1] = *value >> 8;
  tx[2] = *value;
  return dev->spi->rw(dev->spi, tx, NULL, sizeof(tx), dev->nss, dev->spicfg);
}


dac8563_t *dac8563_create(spi_t *bus, gpio_t nss)
{
  dac8563_t *dev = xalloc(sizeof(dac8563_t), 0, MEM_TYPE_DMA);
  dev->spi = bus;
  dev->nss = nss;

  dev->spicfg = bus->get_config(bus, 0, 2000000);

  gpio_conf_output(nss, GPIO_PUSH_PULL,
                   GPIO_SPEED_LOW, GPIO_PULL_NONE);
  gpio_set_output(nss, 1);

  // init
  error_t err = write_cmd(dev, REG_NO_LDAC, NULL);
  if (err) {
    evlog(LOG_ERR, "dac8563: failed to setup LDAC: %s", error_to_string(err));
    free(dev);
    return NULL;
  }
  err = write_cmd(dev, REG_INT_REF, NULL);
  if (err) {
    evlog(LOG_ERR, "dac8563: failed to setup internal ref: %s", error_to_string(err));
    free(dev);
    return NULL;
  }
  return dev;
}


error_t dac8563_power(dac8563_t *dac, uint8_t channel, bool on)
{
  if (channel > 1) {
    return ERR_INVALID_PARAMETER;
  }
  int reg = on ? REG_PWR_UP_A : REG_PWR_DWN_HIZ_A;

  return write_cmd(dac, reg + channel, NULL);
}

error_t dac8563_set_dac(dac8563_t *dac, uint8_t channel, uint16_t value)
{
  if (channel > 1) {
    return ERR_INVALID_PARAMETER;
  }
  return write_cmd(dac, REG_INPUT_A_UPD + channel, &value);
}
