#include <mios/driver.h>
#include <mios/eventlog.h>

void *
driver_probe(driver_type_t type, device_t *parent)
{
  extern unsigned long _driver_array_begin;
  extern unsigned long _driver_array_end;

  const driver_t *d = (void *)&_driver_array_begin;
  const driver_t *e = (void *)&_driver_array_end;

  void *fn;

  for(; d != e; d++) {
    evlog(LOG_DEBUG, "PROBE %p", d->probe);
    fn = d->probe(type, parent);
    if(fn != NULL)
      return fn;
  }
  return NULL;
}
