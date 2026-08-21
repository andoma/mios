#include "tokmas_baro.h"
#include <malloc.h>
#include <unistd.h>
#include <stdio.h>

// Register map is DPS310/DPS368-shaped, NOT Bosch BMP388 -- see tokmas_baro.h
#define REG_PSR_B2     0x00 // PSR/TMP data, 6 bytes burst
#define REG_PRS_CFG    0x06
#define REG_TMP_CFG    0x07
#define REG_MEAS_CFG   0x08
#define REG_CFG_REG    0x09
#define REG_RESET      0x0c
#define REG_ID         0x0d
#define REG_COEF       0x10 // 21 bytes, c0..c40

#define ID_VALUE       0x11
#define RESET_SOFT_RST 0x09

#define MEAS_CFG_COEF_RDY   (1 << 7)
#define MEAS_CFG_SENSOR_RDY (1 << 6)
#define MEAS_CFG_TMP_RDY    (1 << 5)
#define MEAS_CFG_PRS_RDY    (1 << 4)
#define MEAS_CTRL_BACKGROUND_PT 0x07 // continuous pressure+temperature

// PRS_CFG 0xd3: PM_RATE 50/s (1101), PM_PRC 8x oversampling (0011)
// TMP_CFG 0xd0: TMP_RATE 50/s (1101), TMP_PRC single (0000)
// Combined measurement time 50 * (14.8ms + 3.6ms) = 920ms < 1s budget.
// Neither oversampling exceeds 8x so CFG_REG needs no result shift.
#define PRS_CFG_VAL 0xd3
#define TMP_CFG_VAL 0xd0
#define CFG_REG_VAL 0x00

// Scale factors matching the oversampling above, datasheet Table 4
#define SCALE_KP  7864320.0f // 8x
#define SCALE_KT  524288.0f  // 1x (single)

struct tokmas_baro {
  i2c_t *bus;
  uint8_t addr;

  // 2's complement calibration coefficients, sign-extended
  int32_t c0, c1;
  int32_t c00, c10;
  int32_t c01, c11, c20, c21, c30;
  int32_t c31, c40;
};


static int32_t
sext(uint32_t v, int bits)
{
  const uint32_t signbit = 1u << (bits - 1);
  return (v & signbit) ? (int32_t)(v - (signbit << 1)) : (int32_t)v;
}


tokmas_baro_t *
tokmas_baro_create(i2c_t *bus, uint8_t i2c_addr)
{
  tokmas_baro_t *dev = xalloc(sizeof(tokmas_baro_t), 0, 0);
  dev->bus = bus;
  dev->addr = i2c_addr;
  return dev;
}


error_t
tokmas_baro_reset(tokmas_baro_t *dev)
{
  error_t err;

  err = i2c_write_u8(dev->bus, dev->addr, REG_RESET, RESET_SOFT_RST);
  if(err)
    return err;

  // Datasheet: TSensor_rdy max 12ms, TCoef_rdy max 40ms. The chip's I2C
  // interface itself NACKs address bytes for the first couple ms of this
  // window (confirmed with a logic analyzer) -- a read error here just
  // means "still resetting", not a real fault, so keep polling through it.
  uint8_t meas_cfg = 0;
  for(int i = 0; i < 25; i++) {
    usleep(2000);
    if(i2c_read_u8(dev->bus, dev->addr, REG_MEAS_CFG, &meas_cfg))
      continue;
    if((meas_cfg & (MEAS_CFG_COEF_RDY | MEAS_CFG_SENSOR_RDY)) ==
       (MEAS_CFG_COEF_RDY | MEAS_CFG_SENSOR_RDY))
      break;
  }
  if((meas_cfg & (MEAS_CFG_COEF_RDY | MEAS_CFG_SENSOR_RDY)) !=
     (MEAS_CFG_COEF_RDY | MEAS_CFG_SENSOR_RDY))
    return ERR_TIMEOUT;

  uint8_t id;
  err = i2c_read_u8(dev->bus, dev->addr, REG_ID, &id);
  if(err)
    return err;
  if(id != ID_VALUE)
    return ERR_MISMATCH;

  uint8_t cal[21];
  err = i2c_read_bytes(dev->bus, dev->addr, REG_COEF, cal, sizeof(cal));
  if(err)
    return err;

  dev->c0  = sext((cal[0] << 4) | (cal[1] >> 4), 12);
  dev->c1  = sext(((cal[1] & 0xf) << 8) | cal[2], 12);
  dev->c00 = sext((cal[3] << 12) | (cal[4] << 4) | (cal[5] >> 4), 20);
  dev->c10 = sext(((cal[5] & 0xf) << 16) | (cal[6] << 8) | cal[7], 20);
  dev->c01 = sext((cal[8] << 8) | cal[9], 16);
  dev->c11 = sext((cal[10] << 8) | cal[11], 16);
  dev->c20 = sext((cal[12] << 8) | cal[13], 16);
  dev->c21 = sext((cal[14] << 8) | cal[15], 16);
  dev->c30 = sext((cal[16] << 8) | cal[17], 16);
  dev->c31 = sext((cal[18] << 4) | (cal[19] >> 4), 12);
  dev->c40 = sext(((cal[19] & 0xf) << 8) | cal[20], 12);

  err = i2c_write_u8(dev->bus, dev->addr, REG_PRS_CFG, PRS_CFG_VAL);
  if(err)
    return err;
  err = i2c_write_u8(dev->bus, dev->addr, REG_TMP_CFG, TMP_CFG_VAL);
  if(err)
    return err;
  err = i2c_write_u8(dev->bus, dev->addr, REG_CFG_REG, CFG_REG_VAL);
  if(err)
    return err;
  err = i2c_write_u8(dev->bus, dev->addr, REG_MEAS_CFG,
                     MEAS_CTRL_BACKGROUND_PT);
  if(err)
    return err;

  return 0;
}


// Compensation formula verbatim from datasheet sec. 4.6.1 / 4.6.2:
//   Tcomp = c0*0.5 + c1*Traw_sc
//   Pcomp = c00 + c10*Praw_sc + c20*Praw_sc^2 + c30*Praw_sc^3 +
//           c40*Praw_sc^4 + Traw_sc*(c01 + c11*Praw_sc + c21*Praw_sc^2 +
//           c31*Praw_sc^3)
error_t
tokmas_baro_read(tokmas_baro_t *dev, float *pressure_pa, float *temperature_c)
{
  uint8_t meas_cfg;
  error_t err = i2c_read_u8(dev->bus, dev->addr, REG_MEAS_CFG, &meas_cfg);
  if(err)
    return err;
  if((meas_cfg & (MEAS_CFG_TMP_RDY | MEAS_CFG_PRS_RDY)) !=
     (MEAS_CFG_TMP_RDY | MEAS_CFG_PRS_RDY))
    return ERR_NOT_READY;

  uint8_t buf[6];
  err = i2c_read_bytes(dev->bus, dev->addr, REG_PSR_B2, buf, sizeof(buf));
  if(err)
    return err;

  const int32_t praw = sext((buf[0] << 16) | (buf[1] << 8) | buf[2], 24);
  const int32_t traw = sext((buf[3] << 16) | (buf[4] << 8) | buf[5], 24);

  const float praw_sc = praw / SCALE_KP;
  const float traw_sc = traw / SCALE_KT;

  *temperature_c = dev->c0 * 0.5f + dev->c1 * traw_sc;

  const float praw_sc2 = praw_sc * praw_sc;
  const float praw_sc3 = praw_sc2 * praw_sc;
  const float praw_sc4 = praw_sc3 * praw_sc;

  *pressure_pa = dev->c00 + dev->c10 * praw_sc +
    dev->c20 * praw_sc2 + dev->c30 * praw_sc3 + dev->c40 * praw_sc4 +
    traw_sc * (dev->c01 + dev->c11 * praw_sc + dev->c21 * praw_sc2 +
               dev->c31 * praw_sc3);
  return 0;
}


void
tokmas_baro_dump(tokmas_baro_t *dev, stream_t *st)
{
  uint8_t id;
  error_t err = i2c_read_u8(dev->bus, dev->addr, REG_ID, &id);
  if(err) {
    stprintf(st, "ID read failed: %d\n", err);
    return;
  }
  stprintf(st, "ID: 0x%02x\n", id);

  float pressure_pa, temperature_c;
  err = tokmas_baro_read(dev, &pressure_pa, &temperature_c);
  if(err) {
    stprintf(st, "Read failed: %d\n", err);
    return;
  }
  stprintf(st, "Pressure: %8.2f Pa\n", pressure_pa);
  stprintf(st, "Temp:     %6.2f C\n", temperature_c);
}
