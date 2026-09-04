/*
 * AF_UNIX SOCK_STREAM transport for host-side DSIG.
 *
 * Local-machine-only, immune to the self-echo problem IP multicast has
 * (AF_UNIX has no group/loopback concept, so a sender can never get its
 * own frame back), and -- unlike the SOCK_DGRAM version this replaced --
 * connection-oriented: a peer going away is a real, detectable event
 * (EOF/error on its fd) instead of an address that just silently stops
 * answering. SOCK_SEQPACKET would avoid needing to frame messages
 * ourselves, but macOS never implemented it for AF_UNIX, so this uses
 * plain STREAM with a [u32 LE frame_len][u32 LE signal][payload] framing
 * on top.
 *
 * One side binds+listens+accepts (the server, e.g. fcmon, fanning every
 * frame out to all connected clients); the other side connects (the
 * client, e.g. dsig_tool), retrying in the background until the server
 * is up. Which role a dsig_unix_t plays is inferred from which argument
 * to dsig_unix_create() is non-NULL.
 *
 * Usage (server, e.g. fcmon -- accepts any number of clients):
 *   dsig_unix_t *u = dsig_unix_create("/tmp/fcmon.sock", NULL);
 *   dsig_t *bus = dsig_create(dsig_unix_tx, u);
 *   dsig_unix_start(u, bus);
 *
 * Usage (client, e.g. dsig_tool -- one known server to connect to):
 *   dsig_unix_t *u = dsig_unix_create(NULL, "/tmp/fcmon.sock");
 *   dsig_t *bus = dsig_create(dsig_unix_tx, u);
 *   dsig_unix_start(u, bus);
 *
 * Link with: host/dsig.c, host/dsig_unix.c, -lpthread
 */

#pragma once

#include <stddef.h>
#include <stdint.h>

#include "dsig.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct dsig_unix dsig_unix_t;

/* bind_path: run as a server, bind+listen on this path (unlinked on
 * destroy). Fails immediately (returns NULL) if the bind/listen fails.
 * peer_path: run as a client, connecting to this well-known path.
 * Exactly one of bind_path/peer_path must be non-NULL. Returns NULL on
 * failure (server mode only -- a client always "succeeds", connecting
 * lazily in the background; see dsig_unix_start()).
 */
dsig_unix_t *dsig_unix_create(const char *bind_path, const char *peer_path);

/* Spawns the rx thread. Server: accepts any number of clients and calls
 * dsig_input(bus, ...) for every frame from any of them. Client: connects
 * to peer_path, retrying every 500ms until the server is up, and
 * reconnects the same way if the connection ever drops.
 * Returns 0 on success, -1 on failure.
 */
int dsig_unix_start(dsig_unix_t *t, dsig_t *bus);

/* Stops the rx thread, closes all sockets, unlinks the bind path (server
 * only), frees the transport.
 */
void dsig_unix_destroy(dsig_unix_t *t);

/* TX callback matching dsig_tx_fn: sends to every connected peer
 * (server: all connected clients; client: the one server connection, if
 * currently connected -- silently dropped otherwise). Never blocks: a
 * peer whose socket buffer is full (it stopped reading) is disconnected
 * instead, see dsig_unix_tx_stalls().
 */
void dsig_unix_tx(void *opaque, uint32_t signal, const void *data, size_t len);

/* Number of peers dropped so far for not keeping up. */
unsigned int dsig_unix_tx_stalls(const dsig_unix_t *t);

#ifdef __cplusplus
}
#endif
