#pragma once

#include <sys/queue.h>
#include <stdint.h>

#include "error.h"
#include "atomic.h"

struct stream;
struct device;

typedef enum {
  DEVICE_POWER_STATE_SUSPEND,
  DEVICE_POWER_STATE_RESUME,
} device_power_state_t;

typedef struct device_class {
  void (*dc_print_info)(struct device *dev, struct stream *s);
  void (*dc_power_state)(struct device *dev, device_power_state_t state);

  /*
   * Turn off IRQ, etc. Once returned there will be no more callbacks
   * from the device. This is, for example, called before any other
   * cleanup is done by network stack related to the interface.
   */
  error_t (*dc_disable)(struct device *dev);

  /*
   * Shutdown. Hold device in RESET, Unmap IO-resources, etc
   * Basically return hardware to pristine state.
   *
   * This will be called by PCIe controller code etc when shutting down.
   *
   * Note: For network devices this should call netif_detach() which
   * will schedule some tasks on the network thread to avoid various
   * race conditions. The networking code will in turn call
   * dc_disable() before cleanup of routes, nexthops, etc.
   */
  error_t (*dc_shutdown)(struct device *dev);

  /*
   * Release mios resources (free memory)
   */
  void (*dc_dtor)(struct device *dev);
} device_class_t;


#define DEVICE_F_DEBUG 0x1

#define DEVICE_TYPE_UNDEF 0
#define DEVICE_TYPE_PCI   1

typedef struct device {
  const char *d_name;
  const device_class_t *d_class;
  struct device *d_parent;
  STAILQ_ENTRY(device) d_link;
  atomic_t d_refcount;
  uint16_t d_flags;
  uint16_t d_type;
} device_t;

void device_register(device_t *d);

void device_unregister(device_t *d);

void device_release(device_t *d);

void device_retain(device_t *d);

void device_power_state(device_power_state_t state);

error_t device_shutdown(device_t *parent);
