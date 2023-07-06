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

#define SERVICE_TYPE_STREAM 0
#define SERVICE_TYPE_DGRAM  1

typedef struct {
  uint16_t max_fragment_size;
  uint16_t preferred_offset;
} svc_pbuf_policy_t;

typedef struct service {

  const char *name;

  uint16_t id;

  uint16_t type;

  void *(*open)(void *opaque,
                service_event_cb_t *event,
                svc_pbuf_policy_t pbuf_policy,
                service_get_flow_header_t *get_flow_hdr);

  // Data from network to service
  struct pbuf *(*push)(void *opaque, struct pbuf *pb);

  int (*may_push)(void *opaque);

  struct pbuf *(*pull)(void *opaque);

  // Once called, the network engine will not call anything again
  void (*close)(void *opaque);

} service_t;

const service_t *service_find_by_name(const char *name);

const service_t *service_find_by_id(uint32_t id);

#define SERVICE_DEF(name, id, type, open, push, maypush, pull, close)    \
  static const service_t MIOS_JOIN(rpc, __LINE__) __attribute__ ((used, section("servicedef"))) = { name, id, type, open, push, maypush, pull, close };

