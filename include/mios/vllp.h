#pragma once

#include <stdint.h>
#include <stddef.h>

#include <mios/error.h>
#include <mios/pushpull.h>

typedef struct vllp vllp_t;

typedef struct vllp_channel vllp_channel_t;

#ifdef ENABLE_VLLP_CLIENT
typedef struct vllp_bind vllp_bind_t;
#endif

// Largest message the server can reassemble with the pbuf size this
// build is configured for. A bigger message is dropped (and logged), and
// because the sender gets no acknowledgement it will keep retrying, so
// anything that must get through has to fit. Scales with PBUF_DATA_SIZE,
// which is why it is a query rather than a constant.
size_t vllp_max_message_size(void);

vllp_t *vllp_server_create(uint32_t txid, uint32_t rxid, uint8_t mtu,
                           uint8_t timeout_seconds);

#ifdef ENABLE_VLLP_CLIENT

// The client end of a link. Only a client may establish a link and open
// channels (see docs/vllp.txt), so the two ends of an id pair must be
// created with different calls -- one server, one client. A client SYNs
// once a second until the server answers, and re-SYNs after a timeout, so
// this may be called before the peer exists or the bus is up.
//
// `mtu` and `timeout_seconds` must match the peer's; a mismatched MTU is
// rejected during the handshake and logged.
vllp_t *vllp_client_create(uint32_t txid, uint32_t rxid, uint8_t mtu,
                           uint8_t timeout_seconds);

// Open one channel to the remote service `service`, binding it to the
// caller's app functions (fill pp->app and pp->app_opaque first; the rest
// of `pp` is filled in on return). Requires an established link, and the
// channel is not re-opened after link loss -- the app learns about that
// through its pushpull close() callback. Use vllp_client_bind() for
// anything that should simply stay connected.
//
// `service` is referenced, not copied.
error_t vllp_client_channel_open(vllp_t *v, const char *service,
                                 pushpull_t *pp);

// Keep a channel to `service` open for the life of the system: opened when
// the link comes up, re-opened on every later session, and retried on a
// backoff if the peer refuses. `open` is called from net context to bind a
// fresh app to each new channel; the previous app has already had its
// close() callback invoked by then, so each session starts clean.
//
// May be called before the link is up. `service` is referenced, not
// copied. Returns a handle for introspection, or NULL on failure.
vllp_bind_t *vllp_client_bind(vllp_t *v, const char *service,
                              error_t (*open)(void *opaque, pushpull_t *pp),
                              void *opaque);

#endif // ENABLE_VLLP_CLIENT
