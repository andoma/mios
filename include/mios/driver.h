#pragma once

#include <stdint.h>

#include <mios/device.h>
#include <mios/error.h>
#include <mios/mios.h>

typedef enum {
  DRIVER_TYPE_PCI,
  DRIVER_TYPE_ETHPHY,

} driver_type_t;

typedef struct driver {

  void *(*probe)(driver_type_t type, device_t *parent);

} driver_t;

void *driver_probe(driver_type_t type, device_t *parent);

#define DRIVER(probefn, prio)                                             \
static const driver_t MIOS_JOIN(driverdef, __LINE__) __attribute__ ((used, section("driver."#prio))) = { .probe = probefn };

