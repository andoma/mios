#include "icm42688.h"
#include <malloc.h>
#include <unistd.h>
#include <stdio.h>

#define ICM42688_REG_DEVICE_CONFIG  0x11
#define ICM42688_REG_TEMP_DATA1     0x1d // First of 14 contiguous data regs
#define ICM42688_REG_PWR_MGMT0      0x4e
#define ICM42688_REG_GYRO_CONFIG0   0x4f
#define ICM42688_REG_ACCEL_CONFIG0  0x50
#define ICM42688_REG_WHO_AM_I       0x75

#define ICM42688_WHO_AM_I_VALUE     0x47

// +-2000dps / +-16g, 1kHz ODR (== reset default, written explicitly).
// No DRDY line is wired on fc1 (INT1/INT2 unconnected), so this is read
// by polling at the ODR rate rather than off an interrupt.
#define ICM42688_GYRO_CONFIG0_VAL   0x06
#define ICM42688_ACCEL_CONFIG0_VAL  0x06

// rad/s per LSB at +-2000dps (16.4 LSB/(deg/s), datasheet Table 1)
#define ICM42688_GYRO_SCALE   ((1.0f / 16.4f) * (3.14159265f / 180.0f))
// g per LSB at +-16g (2048 LSB/g, datasheet Table "Accelerometer Sensitivity")
#define ICM42688_ACCEL_SCALE  (1.0f / 2048.0f)
// degC = raw / 132.48 + 25 (datasheet "Temperature Sensor" table)
#define ICM42688_TEMP_SCALE   (1.0f / 132.48f)
#define ICM42688_TEMP_OFFSET  25.0f

struct icm42688 {
  spi_t *spi;
  gpio_t nss;
  int spicfg;
  uint8_t buf[1 + 14];
};


static error_t
read_u8(icm42688_t *dev, uint8_t reg, uint8_t *value)
{
  dev->buf[0] = 0x80 | reg;
  dev->buf[1] = 0;
  error_t err = dev->spi->rw(dev->spi, dev->buf, dev->buf, 2, dev->nss,
                             dev->spicfg);
  if(!err)
    *value = dev->buf[1];
  return err;
}


static error_t
write_u8(icm42688_t *dev, uint8_t reg, uint8_t value)
{
  dev->buf[0] = reg;
  dev->buf[1] = value;
  return dev->spi->rw(dev->spi, dev->buf, NULL, 2, dev->nss, dev->spicfg);
}


icm42688_t *
icm42688_create(spi_t *bus, gpio_t nss)
{
  icm42688_t *dev = xalloc(sizeof(icm42688_t), 0, MEM_TYPE_DMA);
  dev->spi = bus;
  dev->nss = nss;
  // Datasheet max is 24MHz; leave some margin for clock tolerance/signal
  // integrity on the real board rather than running at the exact edge.
  dev->spicfg = bus->get_config(bus, 0, 20000000);

  gpio_conf_output(nss, GPIO_PUSH_PULL, GPIO_SPEED_LOW, GPIO_PULL_NONE);
  gpio_set_output(nss, 1);
  return dev;
}


error_t
icm42688_reset(icm42688_t *dev)
{
  error_t err;

  err = write_u8(dev, ICM42688_REG_DEVICE_CONFIG, 0x01); // Soft reset
  if(err)
    return err;
  usleep(2000); // Datasheet: wait >=1ms after soft reset

  uint8_t who;
  err = read_u8(dev, ICM42688_REG_WHO_AM_I, &who);
  if(err)
    return err;
  if(who != ICM42688_WHO_AM_I_VALUE)
    return ERR_MISMATCH;

  err = write_u8(dev, ICM42688_REG_GYRO_CONFIG0, ICM42688_GYRO_CONFIG0_VAL);
  if(err)
    return err;
  err = write_u8(dev, ICM42688_REG_ACCEL_CONFIG0, ICM42688_ACCEL_CONFIG0_VAL);
  if(err)
    return err;

  // Enable accel + gyro in Low Noise mode
  err = write_u8(dev, ICM42688_REG_PWR_MGMT0, 0x0f);
  if(err)
    return err;

  // Datasheet: no register writes for 200us after an OFF->on mode change,
  // plus gyro start-up time (30ms typ, from enable to drive ready)
  usleep(45000);
  return 0;
}


error_t
icm42688_read(icm42688_t *dev, imu_values_t *v)
{
  dev->buf[0] = 0x80 | ICM42688_REG_TEMP_DATA1;
  error_t err = dev->spi->rw(dev->spi, dev->buf, dev->buf, sizeof(dev->buf),
                             dev->nss, dev->spicfg);
  if(err)
    return err;

  const int16_t iax = dev->buf[3]  << 8 | dev->buf[4];
  const int16_t iay = dev->buf[5]  << 8 | dev->buf[6];
  const int16_t iaz = dev->buf[7]  << 8 | dev->buf[8];
  const int16_t igx = dev->buf[9]  << 8 | dev->buf[10];
  const int16_t igy = dev->buf[11] << 8 | dev->buf[12];
  const int16_t igz = dev->buf[13] << 8 | dev->buf[14];

  v->ax = iax * ICM42688_ACCEL_SCALE;
  v->ay = iay * ICM42688_ACCEL_SCALE;
  v->az = iaz * ICM42688_ACCEL_SCALE;

  v->wx = igx * ICM42688_GYRO_SCALE;
  v->wy = igy * ICM42688_GYRO_SCALE;
  v->wz = igz * ICM42688_GYRO_SCALE;
  return 0;
}


void
icm42688_dump(icm42688_t *dev, stream_t *st)
{
  uint8_t who;
  error_t err = read_u8(dev, ICM42688_REG_WHO_AM_I, &who);
  if(err) {
    stprintf(st, "WHO_AM_I read failed: %d\n", err);
    return;
  }
  stprintf(st, "WHO_AM_I: 0x%02x\n", who);

  imu_values_t v = {};
  err = icm42688_read(dev, &v);
  if(err) {
    stprintf(st, "Read failed: %d\n", err);
    return;
  }

  const int16_t iraw = dev->buf[1] << 8 | dev->buf[2];
  const float tempC = iraw * ICM42688_TEMP_SCALE + ICM42688_TEMP_OFFSET;

  stprintf(st, "Accel: %8.4f %8.4f %8.4f  g\n", v.ax, v.ay, v.az);
  stprintf(st, "Gyro:  %8.4f %8.4f %8.4f  rad/s\n", v.wx, v.wy, v.wz);
  stprintf(st, "Temp:  %5.1f C\n", tempC);
}
