#pragma once

#include <sys/queue.h>

#include "error.h"

struct stream;
struct device;

typedef error_t (devfn_t)(struct device *d, ...);



typedef enum {
  DEVICE_POWER_STATE_SUSPEND,
  DEVICE_POWER_STATE_RESUME,
} device_power_state_t;

typedef struct device_class {
  void (*dc_print_info)(struct device *dev, struct stream *s);
  void (*dc_power_state)(struct device *dev, device_power_state_t state);
  void (*dc_dtor)(struct device *dev);
} device_class_t;


#define DEVICE_F_DEBUG 0x1

typedef struct device {
  const char *d_name;
  const device_class_t *d_class;
  STAILQ_ENTRY(device) d_link;
  int d_refcount;
  unsigned int d_flags;
} device_t;

void device_register(device_t *d);

void device_unregister(device_t *d);

void device_release(device_t *d);

void device_retain(device_t *d);

void device_power_state(device_power_state_t state);
