#include <unistd.h>
#include <stdlib.h>
#include <mios.h>

#include "ms5611.h"

#define MS5611_PRESSURE_SENSITIVITY_INDEX                1
#define MS5611_PRESSURE_OFFSET_INDEX                     2
#define MS5611_TEMP_COEFF_OF_PRESSURE_SENSITIVITY_INDEX  3
#define MS5611_TEMP_COEFF_OF_PRESSURE_OFFSET_INDEX       4
#define MS5611_REFERENCE_TEMPERATURE_INDEX               5
#define MS5611_TEMP_COEFF_OF_TEMPERATURE_INDEX           6

struct ms5611 {
  i2c_t *i2c;
  uint16_t coeff[8];
};



static error_t
read_u16(i2c_t *i2c, uint8_t addr, uint8_t reg, uint16_t *result)
{
  uint8_t buf[2];
  error_t err = i2c->rw(i2c, addr, &reg, sizeof(reg), buf, sizeof(buf));
  *result = buf[0] << 8 | buf[1];
  return err;
}


static error_t
cmd(i2c_t *i2c, uint8_t addr, uint8_t reg)
{
  return i2c->rw(i2c, addr, &reg, sizeof(reg), NULL, 0);
}


static error_t
read_u24(i2c_t *i2c, uint8_t addr, uint8_t reg, uint32_t *result)
{
  uint8_t buf[3];
  error_t err = i2c->rw(i2c, addr, &reg, sizeof(reg), buf, sizeof(buf));
  *result = buf[0] << 16 | buf[1] << 8 | buf[2];
  return err;
}



error_t
ms5611_create(i2c_t *bus, ms5611_t **ptr)
{
  error_t err;
  ms5611_t *m = malloc(sizeof(ms5611_t));

  m->i2c = bus;
  for(int i = 0; i < 8; i++) {
    if((err = read_u16(m->i2c, 0x77, 0xa0 + i * 2, m->coeff + i)) != ERR_OK)
      return err;
  }


  *ptr = m;
  return ERR_OK;
}

error_t
ms5611_read(ms5611_t *m, ms5611_value_t *values)
{
  error_t err;
  uint32_t adc_pressure;
  uint32_t adc_temperature;

  if((err = cmd(m->i2c, 0x77, 0x48)) != ERR_OK)
    return err;
  usleep(10000);
  if((err = read_u24(m->i2c, 0x77, 0, &adc_pressure)) != ERR_OK)
    return err;
  if((err = cmd(m->i2c, 0x77, 0x58)) != ERR_OK)
    return err;
  usleep(10000);
  if((err = read_u24(m->i2c, 0x77, 0, &adc_temperature)) != ERR_OK)
    return err;

  int32_t dT, TEMP;
  int64_t OFF, SENS, P, T2, OFF2, SENS2;

  // Difference between actual and reference temperature = D2 - Tref
  dT = (int32_t)adc_temperature - ((int32_t)m->coeff[MS5611_REFERENCE_TEMPERATURE_INDEX] <<8 );

  // Actual temperature = 2000 + dT * TEMPSENS
  TEMP = 2000 + ((int64_t)dT * (int64_t)m->coeff[MS5611_TEMP_COEFF_OF_TEMPERATURE_INDEX] >> 23);

  // Second order temperature compensation
  if(TEMP < 2000) {
    T2 = ( 3 * ( (int64_t)dT  * (int64_t)dT  ) ) >> 33;
    OFF2 = 61 * ((int64_t)TEMP - 2000) * ((int64_t)TEMP - 2000) / 16 ;
    SENS2 = 29 * ((int64_t)TEMP - 2000) * ((int64_t)TEMP - 2000) / 16 ;
    if(TEMP < -1500) {
      OFF2 += 17 * ((int64_t)TEMP + 1500) * ((int64_t)TEMP + 1500) ;
      SENS2 += 9 * ((int64_t)TEMP + 1500) * ((int64_t)TEMP + 1500) ;
    }
  } else {
    T2 = ( 5 * ( (int64_t)dT  * (int64_t)dT  ) ) >> 38;
    OFF2 = 0 ;
    SENS2 = 0 ;
  }

  // OFF = OFF_T1 + TCO * dT
  OFF = ( (int64_t)(m->coeff[MS5611_PRESSURE_OFFSET_INDEX]) << 16 ) + ( ( (int64_t)(m->coeff[MS5611_TEMP_COEFF_OF_PRESSURE_OFFSET_INDEX]) * dT ) >> 7 ) ;
  OFF -= OFF2 ;

  // Sensitivity at actual temperature = SENS_T1 + TCS * dT
  SENS = ( (int64_t)m->coeff[MS5611_PRESSURE_SENSITIVITY_INDEX] << 15 ) + ( ((int64_t)m->coeff[MS5611_TEMP_COEFF_OF_PRESSURE_SENSITIVITY_INDEX] * dT) >> 8 ) ;
  SENS -= SENS2 ;

  // Temperature compensated pressure = D1 * SENS - OFF
  P = ( ( (adc_pressure * SENS) >> 21 ) - OFF ) >> 15 ;

  values->temp = ((float)TEMP - T2) / 100.0f;
  values->pressure = (float)P / 100.0f;

  return ERR_OK;
}
