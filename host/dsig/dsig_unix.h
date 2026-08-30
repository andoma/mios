/*
 * AF_UNIX SOCK_DGRAM transport for host-side DSIG.
 *
 * Message-oriented like UDP (each datagram is exactly one dsig frame,
 * [u32 LE signal][payload]), but local-machine-only and immune to the
 * self-echo problem IP multicast has -- AF_UNIX has no group/loopback
 * concept at all, so a sender can never get its own datagram back.
 *
 * Unlike a UDP multicast group (where the OS fans a datagram out to
 * every member automatically), AF_UNIX has no broadcast: this transport
 * tracks known peers itself. A peer becomes known either by being given
 * explicitly at creation (the "connect to this known server" case, e.g.
 * dsig_tool talking to fcmon) or by being the sender of a received
 * datagram (the "peers register themselves" case, e.g. fcmon, which
 * doesn't know its clients up front) -- either way, every subsequent
 * send goes out to every known peer.
 *
 * Both ends need their own bound socket path so the other side has an
 * address to reply to; dsig_unix_create() always binds one (auto-
 * generated under /tmp if bind_path is NULL), and unlinks it on destroy.
 *
 * Usage (server, e.g. fcmon -- unknown peers, discovered on receive):
 *   dsig_unix_t *u = dsig_unix_create("/tmp/fcmon.sock", NULL);
 *   dsig_t *bus = dsig_create(dsig_unix_tx, u);
 *   dsig_unix_start(u, bus);
 *
 * Usage (client, e.g. dsig_tool -- one already-known peer):
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

/* bind_path: where this end's own socket lives (peers reply here). NULL
 * auto-generates a path under /tmp (based on pid), unlinked on destroy.
 * peer_path: an already-known peer to seed the send list with (e.g. a
 * server's well-known path). NULL if peers should only be discovered by
 * receiving from them. At least one of bind_path/peer_path must be
 * non-NULL. Returns NULL on failure.
 */
dsig_unix_t *dsig_unix_create(const char *bind_path, const char *peer_path);

/* Spawns the rx thread, which calls dsig_input(bus, ...) for every
 * received datagram and adds its sender to the peer list if new.
 * Returns 0 on success, -1 on failure.
 */
int dsig_unix_start(dsig_unix_t *t, dsig_t *bus);

/* Stops the rx thread, closes the socket, unlinks the bind path, frees
 * the transport.
 */
void dsig_unix_destroy(dsig_unix_t *t);

/* TX callback matching dsig_tx_fn: sends to every known peer. */
void dsig_unix_tx(void *opaque, uint32_t signal, const void *data, size_t len);

#ifdef __cplusplus
}
#endif
