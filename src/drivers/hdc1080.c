#include <stdio.h>
#include <mios/io.h>
#include <stdlib.h>
#include <unistd.h>

#include "hdc1080.h"

struct hdc1080 {
  i2c_t *i2c;
};

hdc1080_t *
hdc1080_create(i2c_t *i2c)
{
  // Reset
  uint8_t buf[3] = {0x02, 0x80, 0x00};
  i2c_rw(i2c, 0x40, buf, 3, NULL, 0);

  // Acquire both temperature and humidity
  buf[1] = 0x10;
  int r = i2c_rw(i2c, 0x40, buf, 3, NULL, 0);
  if(r) {
    printf("hdc1080: Failed to init\n");
    return NULL;
  }

  hdc1080_t *hdc = malloc(sizeof(hdc1080_t));
  hdc->i2c = i2c;
  return hdc;

}


error_t
hdc1080_read(hdc1080_t *hdc, int *deci_degrees, int *rh_promille)
{
  uint8_t buf[4];
  error_t err;
  i2c_t *i2c = hdc->i2c;

  buf[0] = 0;
  err = i2c_rw(i2c, 0x40, buf, 1, NULL, 0);
  if(err)
    return err;

  usleep(20000);
  err = i2c_rw(i2c, 0x40, NULL, 0, buf, 4);
  if(err)
    return err;

  const int raw_temp = buf[0] << 8 | buf[1];
  *deci_degrees = (32768 + 10 * 165 * raw_temp) / 65536 - 400;

  const int raw_rh = buf[2] << 8 | buf[3];
  *rh_promille = (32768 + raw_rh * 1000) / 65536;

  return 0;
}
