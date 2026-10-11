#include "imx335.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <mios/io.h>
#include <mios/eventlog.h>
#include <mios/type_macros.h>

// Register values from ST's IMX335 driver (stm32-imx335, BSD-3-Clause,
// Copyright 2022 STMicroelectronics), which is what runs on the
// B-CAMS-IMX module. Multi-byte registers are little endian.

#define REG_STANDBY    0x3000
#define REG_REGHOLD    0x3001
#define REG_VMAX       0x3030 // 20 bit, lines per frame
#define REG_SHR0       0x3058 // 20 bit, shutter
#define REG_GAIN       0x30e8 // 11 bit, 0.3 dB/step
#define REG_SYS_MODE   0x319e // Data rate
#define REG_TPG        0x329e
#define REG_ID         0x3912

#define SHR0_MIN 9

// Data rate with the register settings below (SYS_MODE = 0x01)
#define LANE_MBPS 1188

#define IMX335_WIDTH  2592
#define IMX335_HEIGHT 1944

// Black level at 10 bit scale (BLKLEVEL reset value)
#define IMX335_BLACK_LEVEL 50

// Analog gain range, in 0.3 dB steps (30 dB)
#define MAX_GAIN_CODE 100

// 1H at 30 fps with the default HMAX
#define VMAX_30FPS 4500

typedef struct {
  uint16_t addr;
  uint8_t val;
} regval_t;

// 2592x1944 window cropping mode, INCK 24 MHz
static const regval_t res_2592_1944[] = {
  {0x3000, 0x01}, {0x3002, 0x00}, {0x300c, 0x3b}, {0x300d, 0x2a},
  {0x3018, 0x04}, {0x302c, 0x3c}, {0x302e, 0x20}, {0x3056, 0x98},
  {0x3074, 0xc8}, {0x3076, 0x30}, {0x304c, 0x00}, {0x314c, 0xc6},
  {0x315a, 0x02}, {0x3168, 0xa0}, {0x316a, 0x7e}, {0x31a1, 0x00},
  {0x3288, 0x21}, {0x328a, 0x02}, {0x3414, 0x05}, {0x3416, 0x18},
  {0x3648, 0x01}, {0x364a, 0x04}, {0x364c, 0x04}, {0x3678, 0x01},
  {0x367c, 0x31}, {0x367e, 0x31}, {0x3706, 0x10}, {0x3708, 0x03},
  {0x3714, 0x02}, {0x3715, 0x02}, {0x3716, 0x01}, {0x3717, 0x03},
  {0x371c, 0x3d}, {0x371d, 0x3f}, {0x372c, 0x00}, {0x372d, 0x00},
  {0x372e, 0x46}, {0x372f, 0x00}, {0x3730, 0x89}, {0x3731, 0x00},
  {0x3732, 0x08}, {0x3733, 0x01}, {0x3734, 0xfe}, {0x3735, 0x05},
  {0x3740, 0x02}, {0x375d, 0x00}, {0x375e, 0x00}, {0x375f, 0x11},
  {0x3760, 0x01}, {0x3768, 0x1b}, {0x3769, 0x1b}, {0x376a, 0x1b},
  {0x376b, 0x1b}, {0x376c, 0x1a}, {0x376d, 0x17}, {0x376e, 0x0f},
  {0x3776, 0x00}, {0x3777, 0x00}, {0x3778, 0x46}, {0x3779, 0x00},
  {0x377a, 0x89}, {0x377b, 0x00}, {0x377c, 0x08}, {0x377d, 0x01},
  {0x377e, 0x23}, {0x377f, 0x02}, {0x3780, 0xd9}, {0x3781, 0x03},
  {0x3782, 0xf5}, {0x3783, 0x06}, {0x3784, 0xa5}, {0x3788, 0x0f},
  {0x378a, 0xd9}, {0x378b, 0x03}, {0x378c, 0xeb}, {0x378d, 0x05},
  {0x378e, 0x87}, {0x378f, 0x06}, {0x3790, 0xf5}, {0x3792, 0x43},
  {0x3794, 0x7a}, {0x3796, 0xa1}, {0x37b0, 0x36}, {0x3a00, 0x01},
};

// 2 data lanes, 10 bit AD and output
static const regval_t mode_2l_10b[] = {
  {0x3050, 0x00}, {0x319d, 0x00}, {0x341c, 0xff}, {0x341d, 0x01},
  {0x3a01, 0x01},
};

// No mirror / flip
static const regval_t orientation_normal[] = {
  {0x3074, 0xc8}, {0x3075, 0x00}, {0x304e, 0x00}, {0x304f, 0x00},
  {0x3081, 0x02}, {0x3083, 0x02}, {0x30b6, 0x00}, {0x30b7, 0x00},
  {0x3116, 0x08}, {0x3117, 0x00},
};

static const regval_t tpg_enable[] = {
  {0x3148, 0x10}, {0x3280, 0x00}, {0x329c, 0x01}, {0x32a0, 0x11},
  {0x3302, 0x00}, {0x3303, 0x00}, {0x336c, 0x00},
};

static const regval_t tpg_disable[] = {
  {0x3148, 0x00}, {0x3280, 0x01}, {0x329c, 0x00}, {0x32a0, 0x10},
  {0x3302, 0x32}, {0x3303, 0x00}, {0x336c, 0x01},
};

typedef struct imx335 {
  image_sensor_t is;
  i2c_t *bus;
  uint32_t vmax;
  uint32_t exposure;
  uint8_t gain_code;
  int8_t test_pattern;
  uint8_t sys_mode;
  // Linear gain (256 = 1.0) for each 0.3 dB step
  uint16_t gain_table[MAX_GAIN_CODE + 1];
} imx335_t;


static error_t
imx335_write(imx335_t *s, uint16_t reg, const uint8_t *data, size_t len)
{
  uint8_t buf[2 + 4];
  if(len > 4)
    return ERR_INVALID_ARGS;
  buf[0] = reg >> 8;
  buf[1] = reg;
  for(size_t i = 0; i < len; i++)
    buf[2 + i] = data[i];
  return i2c_rw(s->bus, IMX335_I2C_ADDR, buf, 2 + len, NULL, 0);
}


static error_t
imx335_write_le(imx335_t *s, uint16_t reg, uint32_t value, size_t len)
{
  uint8_t data[4] = { value, value >> 8, value >> 16, value >> 24 };
  return imx335_write(s, reg, data, len);
}


static error_t
imx335_read(imx335_t *s, uint16_t reg, uint8_t *data, size_t len)
{
  uint8_t addr[2] = { reg >> 8, reg };
  return i2c_rw(s->bus, IMX335_I2C_ADDR, addr, 2, data, len);
}


static error_t
imx335_write_table(imx335_t *s, const regval_t *t, size_t count)
{
  for(size_t i = 0; i < count; i++) {
    error_t err = imx335_write(s, t[i].addr, &t[i].val, 1);
    if(err)
      return err;
  }
  return 0;
}

#define WRITE_TABLE(s, t) imx335_write_table(s, t, ARRAYSIZE(t))


static error_t
imx335_start(image_sensor_t *is)
{
  imx335_t *s = (imx335_t *)is;
  uint8_t v = 0;
  error_t err = imx335_write(s, REG_STANDBY, &v, 1);
  if(err)
    return err;
  usleep(20000);
  return 0;
}


static error_t
imx335_stop(image_sensor_t *is)
{
  imx335_t *s = (imx335_t *)is;
  uint8_t v = 1;
  return imx335_write(s, REG_STANDBY, &v, 1);
}


static error_t
imx335_hold(imx335_t *s, int on)
{
  uint8_t v = on;
  return imx335_write(s, REG_REGHOLD, &v, 1);
}


static error_t
imx335_set_exposure(image_sensor_t *is, uint32_t lines)
{
  imx335_t *s = (imx335_t *)is;
  if(lines < 1)
    lines = 1;
  if(lines > is->max_exposure)
    lines = is->max_exposure;

  imx335_hold(s, 1);
  error_t err = imx335_write_le(s, REG_SHR0, s->vmax - lines, 3);
  imx335_hold(s, 0);
  if(!err)
    s->exposure = lines;
  return err;
}


static error_t
imx335_set_gain(image_sensor_t *is, uint32_t gain)
{
  imx335_t *s = (imx335_t *)is;
  int code = 0;
  while(code < MAX_GAIN_CODE && s->gain_table[code + 1] <= gain)
    code++;

  imx335_hold(s, 1);
  error_t err = imx335_write_le(s, REG_GAIN, code, 2);
  imx335_hold(s, 0);
  if(!err)
    s->gain_code = code;
  return err;
}


static error_t
imx335_set_test_pattern(image_sensor_t *is, int pattern)
{
  imx335_t *s = (imx335_t *)is;
  error_t err;
  if(pattern < 0) {
    err = WRITE_TABLE(s, tpg_disable);
  } else if(pattern > 11) {
    return ERR_INVALID_ARGS;
  } else {
    uint8_t v = pattern;
    err = imx335_write(s, REG_TPG, &v, 1);
    if(!err)
      err = WRITE_TABLE(s, tpg_enable);
  }
  if(!err)
    s->test_pattern = pattern < 0 ? -1 : pattern;
  return err;
}


static void
imx335_print_info(image_sensor_t *is, stream_t *st)
{
  imx335_t *s = (imx335_t *)is;
  const int db10 = s->gain_code * 3; // 0.1 dB units
  stprintf(st, "  Exposure: %u/%u lines (%u us)  Gain: %d.%d dB (x%u.%02u)\n",
           s->exposure, is->max_exposure,
           (uint32_t)((uint64_t)s->exposure * is->line_time_ns / 1000),
           db10 / 10, db10 % 10,
           s->gain_table[s->gain_code] / 256,
           s->gain_table[s->gain_code] % 256 * 100 / 256);
  stprintf(st, "  VMAX: %u  SYS_MODE: 0x%02x  Test pattern: ",
           s->vmax, s->sys_mode);
  if(s->test_pattern < 0)
    stprintf(st, "off\n");
  else
    stprintf(st, "%d\n", s->test_pattern);
}


image_sensor_t *
imx335_create(i2c_t *bus, int fps)
{
  imx335_t *s = calloc(1, sizeof(imx335_t));
  s->bus = bus;
  s->test_pattern = -1;

  uint8_t id;
  error_t err = imx335_read(s, REG_ID, &id, 1);
  if(err || id != 0x00) {
    evlog(LOG_ERR, "imx335: Not detected (err:%d id:0x%02x)", err, id);
    free(s);
    return NULL;
  }

  imx335_read(s, REG_SYS_MODE, &s->sys_mode, 1);

  if(fps < 1 || fps > 30)
    fps = 30;
  s->vmax = VMAX_30FPS * 30 / fps;

  if((err = WRITE_TABLE(s, res_2592_1944)) ||
     (err = WRITE_TABLE(s, mode_2l_10b)) ||
     (err = WRITE_TABLE(s, orientation_normal)) ||
     (err = imx335_write_le(s, REG_VMAX, s->vmax, 3))) {
    evlog(LOG_ERR, "imx335: Init failed: %d", err);
    free(s);
    return NULL;
  }

  // 10^(0.3/20) = 1.035142 per step, in 16.16 fixed point
  uint64_t g = 256 << 16;
  for(int i = 0; i <= MAX_GAIN_CODE; i++) {
    s->gain_table[i] = g >> 16;
    g = g * 67839 >> 16;
  }

  image_sensor_t *is = &s->is;
  is->name = "IMX335";
  is->width = IMX335_WIDTH;
  is->height = IMX335_HEIGHT;
  is->fps = fps;
  is->bayer = IMAGE_SENSOR_BAYER_RGGB;
  is->bits = 10;
  is->black_level = IMX335_BLACK_LEVEL;
  is->csi_lanes = 2;
  is->csi_data_type = 0x2b; // RAW10
  is->csi_lane_mbps = LANE_MBPS;
  is->max_exposure = s->vmax - SHR0_MIN;
  is->line_time_ns = 1000000000ULL / (fps * s->vmax);
  is->max_gain = s->gain_table[MAX_GAIN_CODE];
  is->start = imx335_start;
  is->stop = imx335_stop;
  is->set_exposure = imx335_set_exposure;
  is->set_gain = imx335_set_gain;
  is->set_test_pattern = imx335_set_test_pattern;
  is->print_info = imx335_print_info;

  imx335_set_gain(is, 256);
  imx335_set_exposure(is, s->vmax / 2);

  evlog(LOG_INFO, "imx335: %dx%d RAW10 @ %d fps (SYS_MODE=0x%02x)",
        is->width, is->height, fps, s->sys_mode);
  return is;
}
