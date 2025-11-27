#pragma once

#include <mios/error.h>
#include <mios/mios.h>

struct device;

typedef struct driver {

  error_t (*attach)(struct device *pd);

} driver_t;

error_t driver_attach(struct device *d);

#define DRIVER(attachfn, prio)                                             \
static const driver_t MIOS_JOIN(driverdef, __LINE__) __attribute__ ((used, section("driver."#prio))) = { .attach = attachfn };

