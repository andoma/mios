/*
 * Opens a dsig_t bus from a transport address string, so any host tool
 * (dsig_tool, mios-mcp, ...) can be pointed at whichever bus it needs to
 * reach without knowing anything about the concrete transport.
 *
 * kind:
 *   "usb"              -- dsig-over-USB (see dsig_usb.h)
 *   "udp"               -- UDP multicast (see dsig_udp.h)
 *   "cansock" or "can"  -- SocketCAN (see dsig_cansock.h)
 *   "file:///path"      -- AF_UNIX (see dsig_unix.h), e.g. a gateway's
 *                          socket such as fcmon's /tmp/fcmon.sock
 *
 * Link with: dsig.c, dsig_transport.c, dsig_udp.c, dsig_unix.c,
 * dsig_cansock.c, dsig_usb.c, -lusb-1.0, -lpthread
 */

#pragma once

#include <stddef.h>
#include <stdint.h>

#include "dsig.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct dsig_transport dsig_transport_t;

/* Optional tap invoked for every frame actually sent on the underlying
 * transport (e.g. for a "-d" raw dump). Received frames need no such
 * hook: subscribe on *out_bus with dsig_sub() as usual, which works
 * identically regardless of transport.
 */
typedef void (*dsig_transport_debug_fn)(void *opaque, const char *dir,
                                        uint32_t signal, const void *data,
                                        size_t len);

/* group/port/ifname apply to "udp"; ifname to "cansock"; usb_vid/usb_pid/
 * usb_subclass to "usb". Irrelevant args for a given kind are ignored.
 * debug_fn may be NULL. On failure returns NULL (diagnostic already
 * printed to stderr) and *out_bus is untouched.
 */
dsig_transport_t *dsig_transport_open(const char *kind,
                                      const char *group, uint16_t port,
                                      const char *ifname,
                                      uint16_t usb_vid, uint16_t usb_pid,
                                      uint8_t usb_subclass,
                                      dsig_transport_debug_fn debug_fn,
                                      void *debug_opaque,
                                      dsig_t **out_bus);

void dsig_transport_close(dsig_transport_t *t, dsig_t *bus);

#ifdef __cplusplus
}
#endif
