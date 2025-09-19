#include <mios/driver.h>

error_t
driver_attach(struct device *dev)
{
  extern unsigned long _driver_array_begin;
  extern unsigned long _driver_array_end;

  const driver_t *d = (void *)&_driver_array_begin;
  const driver_t *e = (void *)&_driver_array_end;

  error_t rval = ERR_NOT_FOUND;

  for(; d != e; d++) {
    error_t err = d->attach(dev);
    if(!err)
      return 0;
    if(err != ERR_MISMATCH)
      rval = err;
  }
  return rval;
}
