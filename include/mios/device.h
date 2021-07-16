#pragma once

#include <sys/queue.h>

#include "error.h"

struct stream;
struct device;

typedef error_t (devfn_t)(struct device *d, ...);

typedef struct device {
  const char *d_name;
  devfn_t *(*d_getfn)(struct device *dev, int fn);
  void (*d_print_info)(struct device *dev, struct stream *s);
  STAILQ_ENTRY(device) d_link;
} device_t;

void device_register(device_t *d);

error_t device_not_implemented(device_t *d);
