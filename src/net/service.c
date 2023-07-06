#include <mios/service.h>

#include <string.h>


extern unsigned long _servicedef_array_begin;
extern unsigned long _servicedef_array_end;

const service_t *
service_find_by_name(const char *name)
{
  const service_t *s = (void *)&_servicedef_array_begin;
  for(; s != (const void *)&_servicedef_array_end; s++) {
    if(!strcmp(name, s->name))
      return s;
  }
  return NULL;
}

const service_t *
service_find_by_id(uint32_t id)
{
  const service_t *s = (void *)&_servicedef_array_begin;
  for(; s != (const void *)&_servicedef_array_end; s++) {
    if(id == s->id)
      return s;
  }
  return NULL;
}
