/*
 * DSIG: pub/sub signaling on 32-bit signal IDs, host side.
 *
 * Mirrors the guest API in include/mios/dsig.h and src/net/dsig.c.
 * Transport is pluggable: the caller supplies a tx callback and feeds
 * incoming frames in via dsig_input().
 *
 * Link with: host/dsig.c, -lpthread
 */

#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct dsig dsig_t;
typedef struct dsig_sub dsig_sub_t;
typedef struct dsig_emitter dsig_emitter_t;

typedef void (*dsig_tx_fn)(void *opaque, uint32_t signal,
                           const void *data, size_t len);

typedef void (*dsig_rx_cb)(void *opaque, uint32_t signal,
                           const void *data, size_t len);

dsig_t *dsig_create(dsig_tx_fn tx, void *tx_opaque);

void dsig_destroy(dsig_t *d);

/* Transports call this on every received frame. Safe from any thread. */
void dsig_input(dsig_t *d, uint32_t signal, const void *data, size_t len);

/* One-shot publish. Returns 0 on success, -1 on failure (no transport). */
int dsig_send(dsig_t *d, uint32_t signal, const void *data, size_t len);

/* Subscribe. mask is applied as (sig & mask) == signal, matching the
 * guest's dsig_sub. ttl_ms == 0 disables the timeout; otherwise, on
 * expiry the callback fires once with data=NULL, len=0. The TTL rearms
 * on every matching incoming packet.
 */
dsig_sub_t *dsig_sub(dsig_t *d, uint32_t signal, uint32_t mask, int ttl_ms,
                     dsig_rx_cb cb, void *opaque);

void dsig_unsub(dsig_sub_t *s);

/* Periodic emitter. refresh_ms == 0 disables the auto-repeat; the emitter
 * then only fires on dsig_emitter_update(). update() always emits
 * immediately and resets the periodic timer.
 */
dsig_emitter_t *dsig_emitter_create(dsig_t *d, uint32_t signal, int refresh_ms);

void dsig_emitter_update(dsig_emitter_t *e, const void *data, size_t len);

void dsig_emitter_set_refresh(dsig_emitter_t *e, int refresh_ms);

void dsig_emitter_destroy(dsig_emitter_t *e);

#ifdef __cplusplus
}
#endif
