#include "hdc302x.h"

#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>

#include <mios/io.h>
#include <mios/eventlog.h>
#include <mios/climate_zone.h>

#include <util/crc8.h>


/* CRC-8/NRSC-5 for HDC302x
 * poly      = 0x31 (x^8 + x^5 + x^4 + 1)
 * init      = 0xFF
 * refin     = false
 * refout    = false
 * xorout    = 0x00
 */
static uint8_t
hdc302x_crc(const uint8_t *data, size_t len)
{
  uint8_t crc = 0xFF;
  const uint8_t poly = 0x31;

  while (len--) {
    crc ^= *data++;
    for (uint8_t i = 0; i < 8; ++i) {
      if (crc & 0x80) {
        crc = (uint8_t)((crc << 1) ^ poly);
      } else {
        crc <<= 1;
      }
    }
  }

  // no reflection, no final XOR
  return crc;
}


struct hdc302x {
  uint64_t next_read;
  i2c_t *i2c;
  uint8_t addr;
};


hdc302x_t *
hdc302x_create(i2c_t *i2c, uint8_t addr)
{
  error_t err;

  uint8_t tx[2] = {0x37, 0x81};
  uint8_t rx[6];

  err = i2c_rw(i2c, addr, tx, sizeof(tx), rx, 3);
  if(err)
    return NULL;

  if(hdc302x_crc(rx, 2) != rx[2]) {
    evlog(LOG_ERR, "hdc302x: Bad CRC");
    return NULL;
  }

  // 1 measurement / second, low-power-mode = 0
  tx[0] = 0x21;
  tx[1] = 0x30;
  err = i2c_rw(i2c, addr, tx, sizeof(tx), NULL, 0);
  if(err)
    return NULL;

  hdc302x_t *hdc = malloc(sizeof(hdc302x_t));
  hdc->next_read = clock_get() + 1100000;
  hdc->i2c = i2c;
  hdc->addr = addr;
  return hdc;
}


error_t
hdc302x_read(hdc302x_t *hdc, climate_zone_t *cz)
{
  error_t err;

  uint8_t tx[2] = {0xe0, 0x00};
  uint8_t rx[6];

  int64_t now = clock_get();
  if(now < hdc->next_read)
    return 0;

  err = i2c_rw(hdc->i2c, hdc->addr, tx, sizeof(tx), rx, 6);
  if(err) {
    climate_zone_refresh_alert(cz,
                               CLIMATE_ZONE_TEMP_SENSE_ERROR |
                               CLIMATE_ZONE_RH_SENSE_ERROR, 0);
    return err;
  }

  hdc->next_read = now + 1100000;

  const uint16_t traw = (rx[0] << 8) | (rx[1]);
  const uint16_t rraw = (rx[3] << 8) | (rx[4]);

  const float t = -45 + (175.0f / 65535.0f) * traw;
  const float rh = (100.0f / 65535.0f) * rraw;

  climate_zone_set_measured_temperature(cz, t);
  climate_zone_set_measured_rh(cz, rh);

  climate_zone_refresh_alert(cz, 0,
                             CLIMATE_ZONE_TEMP_SENSE_ERROR |
                             CLIMATE_ZONE_RH_SENSE_ERROR);
  return 0;
}
