/*
 * Declarations for the sim-built host VLLP client (vllp.c compiled with
 * VLLP_SIM). The symbols are renamed hvllp_* so they can coexist with the
 * guest server's vllp_* in the same host-mios binary. Suites include this
 * to drive the *real* production host stack in virtual time.
 */
#pragma once

#include <stdint.h>
#include <stddef.h>

// Opaque handles (same underlying structs as vllp.c, alias-compatible with
// the guest's vllp_t so both headers can be included together).
typedef struct vllp hvllp_t;
typedef struct vllp_channel hvllp_channel_t;

#define HVLLP_FDCAN_ADAPTATION 0x1

hvllp_t *hvllp_create_client(int mtu, int timeout, uint32_t flags, void *opaque,
                             void (*tx)(void *opaque, const void *data,
                                        size_t len),
                             void (*log)(void *opaque, int level,
                                         const char *msg));

// The server role, for driving the *guest* client against an independent
// implementation. Layout-identical to vllp.h's open_channel_result_t; the
// two headers deliberately describe the same structs under different
// names so a suite can include both (see the note on hvllp_t above).
//
// In sim mode there is no rx-dispatch thread, so an accepted channel must
// return rx = eof = NULL and be drained with hvllp_channel_read(). Note
// that a read which times out marks the channel closed for good, so a
// peer polls by blocking with a generous deadline rather than spinning on
// short ones.
typedef struct {
  int error;
  void (*rx)(void *opaque, const void *data, size_t length);
  void (*eof)(void *opaque, int error_code);
  void *opaque;
} hvllp_open_channel_result_t;

hvllp_t *hvllp_create_server(int mtu, int timeout, uint32_t flags, void *opaque,
                             void (*tx)(void *opaque, const void *data,
                                        size_t len),
                             void (*log)(void *opaque, int level,
                                         const char *msg),
                             hvllp_open_channel_result_t (*open_channel)(
                                 void *opaque, const char *name,
                                 hvllp_channel_t *vc));
void hvllp_start(hvllp_t *v);
void hvllp_destroy(hvllp_t *v);
int  hvllp_is_connected(hvllp_t *v);

hvllp_channel_t *hvllp_channel_create(hvllp_t *v, const char *name,
                                      uint32_t flags,
                                      void (*rx)(void *, const void *, size_t),
                                      void (*eof)(void *, int),
                                      void (*rdy)(void *),
                                      void *opaque);
void hvllp_channel_send(hvllp_channel_t *vc, const void *data, size_t len);
int  hvllp_channel_read(hvllp_channel_t *vc, void **data, size_t *lenp,
                        long timeout_us);
void hvllp_channel_close(hvllp_channel_t *vc, int error_code, int wait);
int  hvllp_channel_tx_pending(hvllp_channel_t *vc);
int  hvllp_channel_id(hvllp_channel_t *vc);
const char *hvllp_strerror(int error);

// Sim reactor (see vllp_sim.h)
typedef long (*hvllp_recv_fn)(void *tr, uint32_t *id, void *buf, size_t buflen,
                              int64_t deadline);
void hvllp_sim_setup(hvllp_t *v, uint64_t seed, void *tr, hvllp_recv_fn recv,
                     uint32_t rxid);
void hvllp_sim_run(hvllp_t *v, int64_t deadline);
void hvllp_sim_poll(hvllp_t *v, int64_t deadline);
void hvllp_sim_free(void *p);
