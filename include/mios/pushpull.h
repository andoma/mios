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

  // Largest message the transport will take in one go, already rounded
  // down so it occupies whole fragments (see fragment_payload).
  uint16_t max_fragment_size;

  // What one fragment of that message carries, or 0 if the transport
  // does not fragment, plus whatever the transport appends to every
  // message. Together they let a service pick a size that lands on a
  // fragment boundary -- see pushpull_whole_fragments().
  uint16_t fragment_payload;
  uint16_t message_overhead;

  uint16_t preferred_offset;

} pushpull_t;

// The largest message no bigger than len that still occupies whole
// transport fragments.
//
// Worth doing whenever a service picks a size below max_fragment_size.
// A stop-and-wait transport spends a round trip on every fragment
// regardless of how full it is, so a message sized to just overflow one
// pays double to carry barely more: at a 62-byte fragment, 68 bytes go
// out as 62 and 10 and move 34 bytes per round trip where 58 were
// available. Transports that do not fragment leave fragment_payload at
// zero and get their size back unchanged.
static inline size_t
pushpull_whole_fragments(const pushpull_t *pp, size_t len)
{
  const size_t frag = pp->fragment_payload;
  if(frag == 0)
    return len;
  const size_t total = len + pp->message_overhead;
  if(total < frag)
    return len; // already inside a single fragment
  return (total / frag) * frag - pp->message_overhead;
}

struct stream;

// Bind a stream to the app side of a pushpull, for callers that have a
// pushpull and want to do blocking reads and writes on it from a thread.
//
// The server path gets this for free: a service declared with
// SERVICE_DEF_STREAM is handed a stream by service_open_pushpull(). A
// client has no equivalent, because it fills in pp->app itself, so
// without this the only way to drive a client channel is to implement
// the four app callbacks by hand.
//
// Fills in pp->app and pp->app_opaque. The returned stream is the
// caller's to close, and closing it tears the channel down. Returns NULL
// if there is no memory for it.
struct stream *pushpull_stream_create(pushpull_t *pp);

static inline void
pushpull_wakeup(pushpull_t *s, uint32_t flags)
{
  s->net->event(s->net_opaque, flags);
}
