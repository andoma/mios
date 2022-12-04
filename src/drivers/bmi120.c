#include "bmi120.h"
#include <malloc.h>
#include <unistd.h>
#include <stdio.h>

#define BMI120_REG_CHIPID     0x00
#define BMI120_REG_ERR        0x02
#define BMI120_REG_PMU_STATUS 0x03
#define BMI120_REG_ACC_RANGE  0x41
#define BMI120_REG_GYR_RANGE  0x43
#define BMI120_REG_FOC_CONF   0x69
#define BMI120_REG_CMD        0x7e

struct bmi120 {
  spi_t *spi;
  gpio_t nss;
  int spicfg;
  float ascale;
  float wscale;
  uint8_t buf[1 + 12];
};


int
read_u8(bmi120_t *dev, uint8_t reg)
{
  dev->buf[0] = 0x80 | reg;
  dev->buf[1] = 0;
  error_t err = dev->spi->rw(dev->spi, dev->buf, dev->buf, 2, dev->nss,
                             dev->spicfg);
  return err ?: dev->buf[1];
}


error_t
write_u8(bmi120_t *dev, uint8_t reg, uint8_t value)
{
  dev->buf[0] = reg;
  dev->buf[1] = value;
  return dev->spi->rw(dev->spi, dev->buf, NULL, 2, dev->nss, dev->spicfg);
}


bmi120_t *bmi120_create(spi_t *bus, gpio_t nss)
{
  bmi120_t *dev = xalloc(sizeof(bmi120_t), 0, MEM_TYPE_DMA);
  dev->spi = bus;
  dev->nss = nss;

  dev->spicfg = bus->get_config(bus, 0, 2000000);

  gpio_conf_output(nss, GPIO_PUSH_PULL,
                   GPIO_SPEED_LOW, GPIO_PULL_NONE);
  gpio_set_output(nss, 1);

  return dev;
}

static const uint8_t acc_range_reg_value[4] = {
  [BMI120_ACC_RANGE_2G]  = 3,
  [BMI120_ACC_RANGE_4G]  = 5,
  [BMI120_ACC_RANGE_8G]  = 8,
  [BMI120_ACC_RANGE_16G] = 12,
};

static const float acc_range_scaler[4] = {
  [BMI120_ACC_RANGE_2G]  = (1.0f / 16384.0f),
  [BMI120_ACC_RANGE_4G]  = (2.0f / 16384.0f),
  [BMI120_ACC_RANGE_8G]  = (4.0f / 16384.0f),
  [BMI120_ACC_RANGE_16G] = (8.0f / 16384.0f),
};

error_t
bmi120_reset(bmi120_t *dev, bmi120_acc_range_t acc_range)
{
  write_u8(dev, BMI120_REG_CMD, 0x11);
  usleep(100000);
  write_u8(dev, BMI120_REG_CMD, 0x15);
  usleep(100000);
  write_u8(dev, BMI120_REG_CMD, 0x3);
  usleep(100000);
  dev->ascale = acc_range_scaler[acc_range];
  write_u8(dev, BMI120_REG_ACC_RANGE, acc_range_reg_value[acc_range]);
  dev->wscale = (2.0f * 3.141592f) / (1640.0f * 360.0f);
  write_u8(dev, BMI120_REG_GYR_RANGE, 0);
  return 0;
}

void
bmi120_dump(bmi120_t *dev, stream_t *st)
{
  uint8_t reg;
  reg = read_u8(dev, BMI120_REG_CHIPID);
  stprintf(st, "ChipID:0x%x\n", reg);

  reg = read_u8(dev, BMI120_REG_ERR);
  stprintf(st, "ErrorCode:%d FatalError:%d Drop:%d\n",
           (reg >> 1) & 0xf,
           reg & 1,
           (reg  >> 6) & 1);

  reg = read_u8(dev, BMI120_REG_PMU_STATUS);
  stprintf(st, "Accel:%c  Gyro:%c  Magnet:%c\n",
           "SNL?"[(reg >> 4) & 3],
           "SN?F"[(reg >> 2) & 3],
           "SNL?"[(reg     ) & 3]);
}

error_t
bmi120_read(bmi120_t *dev, imu_values_t *v)
{
  dev->buf[0] = 0x8c;
  error_t err = dev->spi->rw(dev->spi, dev->buf, dev->buf, 13, dev->nss,
                             dev->spicfg);
  if(err)
    return err;

  const int16_t igx = dev->buf[2]  << 8 | dev->buf[1];
  const int16_t igy = dev->buf[4]  << 8 | dev->buf[3];
  const int16_t igz = dev->buf[6]  << 8 | dev->buf[5];

  v->wx = igx * dev->wscale;
  v->wy = igy * dev->wscale;
  v->wz = igz * dev->wscale;

  const int16_t iax = dev->buf[8]  << 8 | dev->buf[7];
  const int16_t iay = dev->buf[10] << 8 | dev->buf[9];
  const int16_t iaz = dev->buf[12] << 8 | dev->buf[11];

  v->ax = iax * dev->ascale;
  v->ay = iay * dev->ascale;
  v->az = iaz * dev->ascale;
  return 0;
}
