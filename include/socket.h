#pragma once

#include <stdint.h>
#include <stddef.h>

struct pbuf;

// Functions defined by the application side
typedef struct socket_app_fn {

  __attribute__((warn_unused_result))
  size_t (*push_partial)(void *opaque, struct pbuf *pb);

  // Data from network to service
  // Return an event-mask
  __attribute__((warn_unused_result))
  uint32_t (*push)(void *opaque, struct pbuf *pb);

  __attribute__((warn_unused_result))
  int (*may_push)(void *opaque);

  __attribute__((warn_unused_result))
  struct pbuf *(*pull)(void *opaque);

  // Once called by then network side,
  // The network side will not call anything again
  // reason is only compile-time-constants (no dynamic allocation)
  void (*close)(void *opaque, const char *reason);
} socket_app_fn_t;


#define SOCKET_EVENT_CLOSE  (1 << 0)
#define SOCKET_EVENT_PUSH   (1 << 1)
#define SOCKET_EVENT_PULL   (1 << 2)

#define SOCKET_EVENT_PROTO 16

// Functions defined by the network side
typedef struct socket_net_fn {

  void (*event)(void *opaque, uint32_t signals);

  uint32_t (*get_flow_header)(void *opaque);

} socket_net_fn_t;


typedef struct socket {

  const socket_app_fn_t *app;
  void *app_opaque;

  const socket_net_fn_t *net;
  void *net_opaque;

  uint16_t max_fragment_size;
  uint16_t preferred_offset;

} socket_t;
