#pragma once

#include <stdint.h>
#include <stddef.h>

struct pbuf;

// Functions defined by the application side
typedef struct pushpull_app_fn {

  // Data from network to service
  // Return an event-mask
  __attribute__((warn_unused_result))
  uint32_t (*push)(void *opaque, struct pbuf *pb);

  __attribute__((warn_unused_result))
  int (*may_push)(void *opaque);

  __attribute__((warn_unused_result))
  struct pbuf *(*pull)(void *opaque);

  // Once this is called by the network side,
  // The network side will not call anything again.
  // 'reason' are only compile-time-constant strings (no dynamic allocation)
  void (*close)(void *opaque, const char *reason);
} pushpull_app_fn_t;


#define PUSHPULL_EVENT_CLOSE  (1 << 0)
#define PUSHPULL_EVENT_PUSH   (1 << 1)
#define PUSHPULL_EVENT_PULL   (1 << 2)

#define PUSHPULL_EVENT_PROTO 16

// Functions defined by the network side
typedef struct pushpull_net_fn {

  void (*event)(void *opaque, uint32_t signals);

  uint32_t (*get_flow_header)(void *opaque);

} pushpull_net_fn_t;


typedef struct pushpull {

  const pushpull_app_fn_t *app;
  void *app_opaque;

  const pushpull_net_fn_t *net;
  void *net_opaque;

  uint16_t max_fragment_size;
  uint16_t preferred_offset;

} pushpull_t;

static inline void
pushpull_wakeup(pushpull_t *s, uint32_t flags)
{
  s->net->event(s->net_opaque, flags);
}
