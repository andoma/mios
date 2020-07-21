#include "i2c.h"

error_t  __attribute__((weak))
i2c_rw(i2c_t *i2c, uint8_t addr,
       const uint8_t *write, size_t write_len,
       uint8_t *read, size_t read_len)
{
  return ERR_TIMEOUT;
}
