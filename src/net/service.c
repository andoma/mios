#include <mios/service.h>

#include <string.h>


extern unsigned long _servicedef_array_begin;
extern unsigned long _servicedef_array_end;

const service_t *
service_find(const char *name)
{
  const service_t *s = (void *)&_servicedef_array_begin;
  for(; s != (const void *)&_servicedef_array_end; s++) {
    if(!strcmp(name, s->name))
      return s;
  }
  return NULL;
}
