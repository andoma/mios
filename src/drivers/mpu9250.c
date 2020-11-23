#include <string.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <malloc.h>

#include <mios/mios.h>
#include <mios/task.h>

#include "irq.h"

#include "mpu9250.h"
#include "mpu9250_reg.h"

#define FIFO_ITEM_SIZE  (12 + 8)

struct mpu9250 {
  struct i2c aux;
  spi_t *spi;
  task_waitable_t waitable;
  int pending_irq;
  gpio_t nss;
  float gs;
  float as;
  float msx;
  float msy;
  float msz;
  uint8_t buf[1 + FIFO_ITEM_SIZE];
};


int
read_u8(mpu9250_t *dev, uint8_t reg)
{
  dev->buf[0] = 0x80 | reg;
  dev->buf[1] = 0;
  error_t err = dev->spi->rw(dev->spi, dev->buf, dev->buf, 2, dev->nss);
  return err ?: dev->buf[1];
}


int
read_u16(mpu9250_t *dev, uint8_t reg)
{
  dev->buf[0] = 0x80 | reg;
  dev->buf[1] = 0;
  dev->buf[2] = 0;
  error_t err = dev->spi->rw(dev->spi, dev->buf, dev->buf, 3, dev->nss);
  return err ?: dev->buf[1] << 8 | dev->buf[2];
}


error_t
read_block(mpu9250_t *dev, uint8_t reg, uint8_t *buf, size_t len)
{
  dev->buf[0] = 0x80 | reg;
  memset(dev->buf + 1, 0, len);

  error_t err = dev->spi->rw(dev->spi, dev->buf, dev->buf, len + 1, dev->nss);
  if(err)
    return err;
  memcpy(buf, dev->buf + 1, len);
  return ERR_OK;
}


error_t
write_u8(mpu9250_t *dev, uint8_t reg, uint8_t value)
{
  dev->buf[0] = reg;
  dev->buf[1] = value;
  return dev->spi->rw(dev->spi, dev->buf, NULL, 2, dev->nss);
}


static void
mpu9250_irq(void *arg)
{
  mpu9250_t *dev = arg;
  dev->pending_irq = 1;
  task_wakeup(&dev->waitable, 0);
}


static error_t
aux_i2c(struct i2c *i2c, uint8_t addr,
        const uint8_t *write, size_t write_len,
        uint8_t *read, size_t read_len)
{
  mpu9250_t *dev = (mpu9250_t *)i2c;
  error_t err;

  uint8_t ctrl = 0xa0;
  if(write_len > 0) {
    if((err = write_u8(dev, MPU9250_I2C_SLV4_REG, write[0])))
      return err;
    ctrl = 0x80;
    write++;
    write_len--;
  }

  if(write_len) {
    if((err = write_u8(dev, MPU9250_I2C_SLV4_ADDR, addr)))
      return err;

    for(size_t i = 0; i < write_len; i++) {

      if((err = write_u8(dev, MPU9250_I2C_SLV4_DO, write[i])))
        return err;

      if((err = write_u8(dev, MPU9250_I2C_SLV4_CTRL, ctrl)))
        return err;

      ctrl = 0xa0;

      while(1) {
        uint8_t r = read_u8(dev, MPU9250_I2C_MST_STATUS);
        if(r & 0x40)
          break;
      }
    }
  }

  if(read_len) {
    if((err = write_u8(dev, MPU9250_I2C_SLV4_ADDR, addr | 0x80)))
      return err;

    for(size_t i = 0; i < read_len; i++) {
      if((err = write_u8(dev, MPU9250_I2C_SLV4_CTRL, ctrl)))
        return err;

      ctrl = 0xa0;

      while(1) {
        uint8_t r = read_u8(dev, MPU9250_I2C_MST_STATUS);
        if(r & 0x40)
          break;
      }

      int r = read_u8(dev, MPU9250_I2C_SLV4_DI);
      read[i] = r;
      if(r < 0)
        return r;
    }
  }
  write_u8(dev, MPU9250_I2C_SLV4_CTRL, 0);
  return ERR_OK;
}



mpu9250_t *
mpu9250_create(spi_t *bus, gpio_t nss, gpio_t irq)
{
  mpu9250_t *dev = xalloc(sizeof(mpu9250_t), 0, MEM_TYPE_DMA);
  dev->aux.rw = aux_i2c;
  dev->pending_irq = 0;
  task_waitable_init(&dev->waitable, "mpu9250");
  dev->spi = bus;
  dev->nss = nss;

  gpio_set_output(nss, 1);
  gpio_conf_output(nss, GPIO_PUSH_PULL,
                   GPIO_SPEED_HIGH, GPIO_PULL_NONE);

  gpio_conf_irq(irq, GPIO_PULL_NONE, mpu9250_irq, dev,
                GPIO_RISING_EDGE, IRQ_LEVEL_IO);

  return dev;
}

typedef struct {
  uint8_t reg;
  uint8_t value;
} regval_t;

static const regval_t mpu9250_reg_init[] = {
  { MPU9250_I2C_MST_CTRL,       0x40 },
  { MPU9250_I2C_MST_DELAY_CTRL, 0x01 },
  { MPU9250_I2C_SLV0_CTRL,      0x00 },
  { MPU9250_I2C_SLV1_CTRL,      0x00 },
  { MPU9250_I2C_SLV2_CTRL,      0x00 },
  { MPU9250_I2C_SLV3_CTRL,      0x00 },
  { MPU9250_SMPLRT_DIV,         0x00 },
  { MPU9250_CONFIG,             0x01 },
  { MPU9250_GYRO_CONFIG,        0x00 },
  { MPU9250_ACCEL_CONFIG_2,     0x03 },
  { MPU9250_USER_CTRL,          0x60 },
  { MPU9250_INT_ENABLE,         0x01 },
};



static const regval_t mpu9250_reg_start[] = {
  { MPU9250_I2C_SLV4_CTRL,      0x09 },
  { MPU9250_I2C_SLV0_ADDR,      0x8c },
  { MPU9250_I2C_SLV0_REG,       0x02 },
  { MPU9250_I2C_SLV0_CTRL,      0x88 },
  { MPU9250_FIFO_EN,            0x79 }
};



static error_t
write_multiple_regs(mpu9250_t *dev, const regval_t *regs, size_t count)
{
  error_t err;
  for(size_t i = 0; i < count; i++) {
    if((err = write_u8(dev, regs[i].reg, regs[i].value)))
      return err;
  }
  return ERR_OK;
}



error_t
configure_magnetometer(mpu9250_t *dev)
{
  i2c_t *i2c = &dev->aux;
  const uint8_t addr = 0x0c;
  error_t err;
  uint8_t id;

  if((err = i2c_read_u8(i2c, addr, AK8963_WIA, &id)))
    return err;

  if(id != AK8963_WHO_AM_I_RESULT)
    return ERR_INVALID_ID;

  // Power down magnetometer
  if((err = i2c_write_u8(i2c, addr, AK8963_CNTL, 0x00)))
    return err;
  usleep(10000);

  if((err = i2c_write_u8(i2c, addr, AK8963_CNTL, 0x0f)))
    return err;
  usleep(10000);

  uint8_t calib[3];

  if((err = i2c_read_bytes(i2c, addr, AK8963_ASAX, calib, 3)))
    return err;

  const float s = 10 * 4912 / 32760.0;
  dev->msx = s * (((calib[0] - 128) / 256.0f) + 1.0f);
  dev->msy = s * (((calib[1] - 128) / 256.0f) + 1.0f);
  dev->msz = s * (((calib[2] - 128) / 256.0f) + 1.0f);

  printf("ak8963: Magnetometer calibration: %f %f %f\n",
         dev->msx, dev->msy, dev->msz);

  if((err = i2c_write_u8(i2c, addr, AK8963_CNTL, 0x00)))
    return err;
  usleep(10000);

  // 16 bit resolution, 100 Hz readout
  if((err = i2c_write_u8(i2c, addr, AK8963_CNTL, 0x16)))
    return err;
  usleep(10000);
  return 0;
}





error_t
mpu9250_reset(mpu9250_t *dev)
{
  error_t err;

  // Toggle reset bit
  if((err = write_u8(dev, MPU9250_PWR_MGMT_1, 0x80)))
     return err;

  usleep(10000);
  if((err = write_u8(dev, MPU9250_PWR_MGMT_1, 0x00)))
    return err;
  usleep(100000);

  if(read_u8(dev, MPU9250_WHO_AM_I) != MPU9250_WHO_AM_I_RESULT)
    return ERR_INVALID_ID;

  // Configure main device

  if((err = write_multiple_regs(dev, mpu9250_reg_init,
                                ARRAYSIZE(mpu9250_reg_init)))) {
    return err;
  }

  err = configure_magnetometer(dev);
  if(err) {
    printf("Failed to configure magnetometer: %d\n", err);
    return err;
  }

  dev->gs = 250 / 32767.5;
  dev->as = 1.0 / 16384.0;

  return ERR_OK;
}



static error_t
mpu9250_read_fifo(mpu9250_t *dev, uint8_t *output)
{
  int s = irq_forbid(IRQ_LEVEL_IO);
  while(1) {
    int cnt = read_u16(dev, MPU9250_FIFO_COUNTH);
    if(cnt < 0) {
      irq_permit(s);
      return cnt;
    }
    if(cnt == 0) {
      if(task_sleep_delta(&dev->waitable, 100000, 0)) {
        irq_permit(s);
        return ERR_TIMEOUT;
      }
      dev->pending_irq = 0;
      read_u8(dev, MPU9250_INT_STATUS);
      continue;
    }

    error_t err = read_block(dev, MPU9250_FIFO_R_W, output, FIFO_ITEM_SIZE);
    irq_permit(s);
    return err;
  }
}




error_t
mpu9250_calibrate(mpu9250_t *dev)
{
  error_t err;

  uint8_t buf[FIFO_ITEM_SIZE];

  int32_t agx = 0;
  int32_t agy = 0;
  int32_t agz = 0;
  int calibration_steps = 100;

  if((err = write_multiple_regs(dev, mpu9250_reg_start,
                                ARRAYSIZE(mpu9250_reg_start)))) {
    return err;
  }

  printf("mpu9250: Calibrating...\n");

  for(int i = 0; i < calibration_steps; i++) {
    if((err = mpu9250_read_fifo(dev, buf)))
      return err;

    const int16_t igx = buf[6]  << 8 | buf[7];
    const int16_t igy = buf[8]  << 8 | buf[9];
    const int16_t igz = buf[10] << 8 | buf[11];

    agx += igx;
    agy += igy;
    agz += igz;
  }

  agx /= calibration_steps * -4;
  agy /= calibration_steps * -4;
  agz /= calibration_steps * -4;

  write_u8(dev, MPU9250_XG_OFFSET_H, agx >> 8);
  write_u8(dev, MPU9250_XG_OFFSET_L, agx);
  write_u8(dev, MPU9250_YG_OFFSET_H, agy >> 8);
  write_u8(dev, MPU9250_YG_OFFSET_L, agy);
  write_u8(dev, MPU9250_ZG_OFFSET_H, agz >> 8);
  write_u8(dev, MPU9250_ZG_OFFSET_L, agz);

  printf("mpu9250: Calibration done, bias: %d %d %d\n", agx, agy, agz);
  return ERR_OK;
}


error_t
mpu9250_read(mpu9250_t *dev, mpu9250_values_t *v)
{
  error_t err;
  uint8_t buf[FIFO_ITEM_SIZE];

  if((err = mpu9250_read_fifo(dev, buf)))
    return err;

  const int16_t igx = buf[6]  << 8 | buf[7];
  const int16_t igy = buf[8]  << 8 | buf[9];
  const int16_t igz = buf[10] << 8 | buf[11];

  v->gx = igx * dev->gs;
  v->gy = igy * dev->gs;
  v->gz = igz * dev->gs;

  const int16_t iax = buf[0] << 8 | buf[1];
  const int16_t iay = buf[2] << 8 | buf[3];
  const int16_t iaz = buf[4] << 8 | buf[5];

  v->ax = iax * dev->as;
  v->ay = iay * dev->as;
  v->az = iaz * dev->as;

  const int16_t imx = buf[14] << 8 | buf[13];
  const int16_t imy = buf[16] << 8 | buf[15];
  const int16_t imz = buf[18] << 8 | buf[17];

  v->mx = imx * dev->msx;
  v->my = imy * dev->msy;
  v->mz = imz * dev->msz;

  return ERR_OK;
}
