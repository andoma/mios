/*
 * A "peer" is the thing under test seen from the host VLLP client:
 * either mios running in QEMU (peer_qemu.c) or the host VLLP server
 * in-process (peer_sim.c). Both expose the same handle so scenarios are
 * backend agnostic.
 *
 * Every frame in both directions passes through a linksim_t so faults
 * (drop/dup/delay/corrupt/blackout) can be injected on any backend.
 */
#pragma once

#include <pthread.h>
#include <stdint.h>

#include "vllp.h"
#include "linksim.h"

#define PEER_DIR_C2S 0   /* host client -> server under test */
#define PEER_DIR_S2C 1   /* server under test -> host client */

typedef struct peer_server_status {
  int connected;        /* server considers the link up             */
  int user_channels;    /* open channels on the server, excluding CMC */
  int panicked;         /* server crashed (QEMU: PANIC on console)  */
  char detail[4096];    /* raw text, for logs                        */
} peer_server_status_t;

typedef struct peer peer_t;

struct peer {
  const char *name;
  int mtu;              /* MTU as passed to vllp_create_client() */
  int timeout;          /* link timeout in seconds               */
  uint32_t vllp_flags;
  int has_log_service;
  size_t server_fragment_size; /* length of every chargen message */
  size_t max_message_size;     /* 0 = unlimited; larger -> server closes channel */
  int reliable_blackout;       /* 1 = link sim fully isolates the peer's rx.
                                  0 = qemu: shared multicast can't cleanly
                                  black out the guest's receive path, so the
                                  server-side timeout check is skipped (the
                                  MCU timeout is covered standalone). */

  vllp_t *v;            /* current client; replaced by restart_client */
  linksim_t *ls;

  /* Capture of the host vllp log callback */
  pthread_mutex_t log_mutex;
  int log_count[8];
  char log_last[8][256];

  void *priv;

  /* Tear down the client and create a fresh one (new SYN cookie).
   * All channels must have been closed by the caller. */
  int (*restart_client)(peer_t *p);

  int (*server_status)(peer_t *p, peer_server_status_t *st);

  /* Run a CLI command on the server console, NULL if unsupported */
  int (*server_console)(peer_t *p, const char *cmd, char *out, size_t outlen);

  void (*destroy)(peer_t *p);
};

/* Shared helpers (peer.c) */
void peer_init_common(peer_t *p);
void peer_log_cb(void *opaque, int level, const char *msg);
void peer_log_reset(peer_t *p);
int peer_log_count_at_most(peer_t *p, int max_level); /* e.g. LOG_WARNING */
const char *peer_log_last(peer_t *p, int level);

void peer_set_faults(peer_t *p, int dir, const fault_cfg_t *cfg);
void peer_set_faults_both(peer_t *p, const fault_cfg_t *cfg);
void peer_clear_faults(peer_t *p);

int peer_wait_connected(peer_t *p, int64_t timeout_us);

/* Backends */
typedef struct peer_qemu_cfg {
  const char *qemu_bin;     /* default qemu-system-arm */
  const char *firmware;     /* ELF */
  const char *logdir;       /* console + stderr logs go here */
  uint32_t txid, rxid;      /* host point of view */
  int mtu;
  int timeout;
  int udp_port;             /* DSIG UDP port, default 0xd516 */
} peer_qemu_cfg_t;

peer_t *peer_qemu_create(const peer_qemu_cfg_t *cfg, uint64_t seed);

peer_t *peer_sim_create(int mtu, int timeout, uint64_t seed);
