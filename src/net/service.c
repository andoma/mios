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
service_find_by_namelen(const char *name, size_t len)
{
  const service_t *s = (void *)&_servicedef_array_begin;
  for(; s != (const void *)&_servicedef_array_end; s++) {
    size_t slen = strlen(s->name);
    if(slen == len && !memcmp(name, s->name, len))
      return s;
  }
  return NULL;
}

const service_t *
service_find_by_ble_psm(uint8_t psm)
{
  const service_t *s = (void *)&_servicedef_array_begin;
  for(; s != (const void *)&_servicedef_array_end; s++) {
    if(psm == s->ble_psm)
      return s;
  }
  return NULL;
}

const service_t *
service_find_by_ip_port(uint16_t port)
{
  const service_t *s = (void *)&_servicedef_array_begin;
  for(; s != (const void *)&_servicedef_array_end; s++) {
    if(port == s->ip_port)
      return s;
  }
  return NULL;
}
