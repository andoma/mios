#pragma once

#include "mios.h"
#include "error.h"

#include <stdint.h>
#include <stddef.h>

struct pbuf;

#define SERVICE_EVENT_WAKEUP 0x1
#define SERVICE_EVENT_CLOSE  0x2

typedef void (service_event_cb_t)(void *opaque, uint32_t signals);

typedef uint32_t (service_get_flow_header_t)(void *opaque);

typedef struct service {

  const char *name;

  void *(*open)(void *opaque,
                service_event_cb_t *event,
                size_t max_fragment_size,
                service_get_flow_header_t *get_flow_hdr);

  // Data from network to service
  struct pbuf *(*push)(void *opaque, struct pbuf *pb);

  int (*may_push)(void *opaque);

  struct pbuf *(*pull)(void *opaque);

  // Once called, the network engine will not call anything again
  void (*close)(void *opaque);

} service_t;

const service_t *service_find(const char *name);

#define SERVICE_DEF(name, open, push, maypush, pull, close)        \
  static const service_t MIOS_JOIN(rpc, __LINE__) __attribute__ ((used, section("servicedef"))) = { name, open, push, maypush, pull, close };

