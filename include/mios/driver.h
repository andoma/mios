#pragma once

#include <stdint.h>

#include <mios/device.h>
#include <mios/error.h>
#include <mios/mios.h>

#define DRIVER_TYPE_PCI    1
#define DRIVER_TYPE_ETHPHY 2

typedef struct driver {

  error_t (*probe)(uint16_t type, void *metadata);

} driver_t;

error_t driver_probe(uint16_t type, void *metadata);

#define DRIVER(probefn, prio)                                             \
static const driver_t MIOS_JOIN(driverdef, __LINE__) __attribute__ ((used, section("driver."#prio))) = { .probe = probefn };

