#include <stdio.h>
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
  spi_t *spi;
  uint16_t coeff[8];
  gpio_t nss;
};



static error_t
read_u16(ms5611_t *dev, uint8_t reg, uint16_t *result)
{
  uint8_t tx[3] = {reg};
  uint8_t rx[3];
  error_t err = dev->spi->rw(dev->spi, tx, rx, 3, dev->nss);
  *result = rx[1] << 8 | rx[2];
  return err;
}

static error_t
cmd(ms5611_t *dev, uint8_t reg)
{
  return dev->spi->rw(dev->spi, &reg, NULL, 1, dev->nss);
}


static error_t
read_u24(ms5611_t *dev, uint8_t reg, uint32_t *result)
{
  uint8_t tx[4] = {reg};
  uint8_t rx[4];
  error_t err = dev->spi->rw(dev->spi, tx, rx, 4, dev->nss);
  *result = rx[1] << 16 | rx[2] << 8 | rx[3];
  return err;
}


ms5611_t *
ms5611_create(spi_t *bus, gpio_t nss)
{
  ms5611_t *m = malloc(sizeof(ms5611_t));
  m->spi = bus;
  m->nss = nss;

  gpio_set_output(nss, 1);
  gpio_conf_output(nss, GPIO_PUSH_PULL,
                   GPIO_SPEED_HIGH, GPIO_PULL_NONE);

  return m;
}


int
crc4(const uint16_t *prom)
{
  uint16_t rem = 0;
  const uint8_t *d = (const uint8_t *)prom;
  for(int i = 0; i < 16; i++) {
    if(i != 15)
      rem ^= d[i];

    for(int j = 0; j < 8; j++) {
      rem = (rem << 1) ^ (rem & 0x8000 ? 0x3000 : 0);
    }
  }
  return (rem >> 12) == (prom[7] & 0xf);
}




error_t
ms5611_init(ms5611_t *dev)
{
  error_t err;
  for(int i = 0; i < 8; i++) {
    if((err = read_u16(dev, 0xa0 + i * 2, dev->coeff + i)) != ERR_OK)
      return err;
  }

  if(!crc4(dev->coeff)) {
    printf("ms5611: Invalid CRC\n");
    return ERR_INVALID_ID;
  }
  printf("ms5611: Initialized ok\n");
  return ERR_OK;
}


error_t
ms5611_read(ms5611_t *m, ms5611_value_t *values)
{
  error_t err;
  uint32_t adc_pressure;
  uint32_t adc_temperature;

  if((err = cmd(m, 0x48)) != ERR_OK)
    return err;
  usleep(10000);
  if((err = read_u24(m, 0, &adc_pressure)) != ERR_OK)
    return err;
  if((err = cmd(m, 0x58)) != ERR_OK)
    return err;
  usleep(10000);
  if((err = read_u24(m, 0, &adc_temperature)) != ERR_OK)
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
