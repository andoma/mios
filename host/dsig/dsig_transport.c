#include "dsig_transport.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <strings.h>

#include "dsig_udp.h"
#include "dsig_unix.h"
#include "dsig_cansock.h"
#include "dsig_usb.h"

#define UNIX_SOCKET_PREFIX "file://"

struct dsig_transport {
  dsig_udp_t *udp;
  dsig_cansock_t *can;
  dsig_usb_t *usbt;
  dsig_unix_t *unx;

  // Set only when a debug_fn was given: the real per-backend tx, stashed
  // so the wrapper below can dump-then-forward instead of dsig_create()
  // wiring straight to it.
  dsig_transport_debug_fn debug_fn;
  void *debug_opaque;
  dsig_tx_fn real_tx;
  void *real_tx_opaque;
};

static void
debug_tx_wrap(void *opaque, uint32_t signal, const void *data, size_t len)
{
  dsig_transport_t *t = opaque;
  t->debug_fn(t->debug_opaque, "TX", signal, data, len);
  t->real_tx(t->real_tx_opaque, signal, data, len);
}

dsig_transport_t *
dsig_transport_open(const char *kind,
                    const char *group, uint16_t port, const char *ifname,
                    uint16_t usb_vid, uint16_t usb_pid, uint8_t usb_subclass,
                    dsig_transport_debug_fn debug_fn, void *debug_opaque,
                    dsig_t **out_bus)
{
  dsig_transport_t *t = calloc(1, sizeof(*t));
  t->debug_fn = debug_fn;
  t->debug_opaque = debug_opaque;

  if(!strncmp(kind, UNIX_SOCKET_PREFIX, strlen(UNIX_SOCKET_PREFIX))) {
    const char *path = kind + strlen(UNIX_SOCKET_PREFIX);
    t->unx = dsig_unix_create(NULL, path);
    if(t->unx == NULL) {
      fprintf(stderr, "dsig_transport: failed to open unix socket transport (%s)\n",
              path);
      free(t);
      return NULL;
    }
    if(debug_fn) {
      t->real_tx = dsig_unix_tx;
      t->real_tx_opaque = t->unx;
      *out_bus = dsig_create(debug_tx_wrap, t);
    } else {
      *out_bus = dsig_create(dsig_unix_tx, t->unx);
    }
    if(*out_bus == NULL || dsig_unix_start(t->unx, *out_bus) < 0) {
      if(*out_bus) dsig_destroy(*out_bus);
      dsig_unix_destroy(t->unx);
      free(t);
      return NULL;
    }
    return t;
  }

  if(!strcasecmp(kind, "udp")) {
    t->udp = dsig_udp_create(group, port, ifname);
    if(t->udp == NULL) {
      fprintf(stderr, "dsig_transport: failed to open UDP transport\n");
      free(t);
      return NULL;
    }
    if(debug_fn) {
      t->real_tx = dsig_udp_tx;
      t->real_tx_opaque = t->udp;
      *out_bus = dsig_create(debug_tx_wrap, t);
    } else {
      *out_bus = dsig_create(dsig_udp_tx, t->udp);
    }
    if(*out_bus == NULL || dsig_udp_start(t->udp, *out_bus) < 0) {
      if(*out_bus) dsig_destroy(*out_bus);
      dsig_udp_destroy(t->udp);
      free(t);
      return NULL;
    }
    return t;
  }

  if(!strcasecmp(kind, "cansock") || !strcasecmp(kind, "can")) {
    t->can = dsig_cansock_create(ifname);
    if(t->can == NULL) {
      fprintf(stderr, "dsig_transport: failed to open cansock transport (ifname=%s)\n",
              ifname ? ifname : "can0");
      free(t);
      return NULL;
    }
    if(debug_fn) {
      t->real_tx = dsig_cansock_tx;
      t->real_tx_opaque = t->can;
      *out_bus = dsig_create(debug_tx_wrap, t);
    } else {
      *out_bus = dsig_create(dsig_cansock_tx, t->can);
    }
    if(*out_bus == NULL || dsig_cansock_start(t->can, *out_bus) < 0) {
      if(*out_bus) dsig_destroy(*out_bus);
      dsig_cansock_destroy(t->can);
      free(t);
      return NULL;
    }
    return t;
  }

  if(!strcasecmp(kind, "usb")) {
    t->usbt = dsig_usb_create(usb_vid, usb_pid, usb_subclass);
    if(t->usbt == NULL) {
      fprintf(stderr, "dsig_transport: failed to open USB transport\n");
      free(t);
      return NULL;
    }
    if(debug_fn) {
      t->real_tx = dsig_usb_tx;
      t->real_tx_opaque = t->usbt;
      *out_bus = dsig_create(debug_tx_wrap, t);
    } else {
      *out_bus = dsig_create(dsig_usb_tx, t->usbt);
    }
    if(*out_bus == NULL || dsig_usb_start(t->usbt, *out_bus) < 0) {
      if(*out_bus) dsig_destroy(*out_bus);
      dsig_usb_destroy(t->usbt);
      free(t);
      return NULL;
    }
    return t;
  }

  fprintf(stderr, "dsig_transport: unknown transport: %s\n", kind);
  free(t);
  return NULL;
}

void
dsig_transport_close(dsig_transport_t *t, dsig_t *bus)
{
  if(t->udp)  dsig_udp_destroy(t->udp);
  if(t->can)  dsig_cansock_destroy(t->can);
  if(t->usbt) dsig_usb_destroy(t->usbt);
  if(t->unx)  dsig_unix_destroy(t->unx);
  if(bus)     dsig_destroy(bus);
  free(t);
}
