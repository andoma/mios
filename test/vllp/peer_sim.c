/*
 * In-process peer: host VLLP client <-> linksim <-> host reference server.
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "peer.h"
#include "refserver.h"
#include "tst.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>

typedef struct sim_priv {
  refserver_t *rs;
  pthread_mutex_t client_mutex;   /* guards p->v during restart */
} sim_priv_t;

static void
deliver_to_server(void *opaque, const void *data, size_t len)
{
  peer_t *p = opaque;
  sim_priv_t *sp = p->priv;
  vllp_input(refserver_vllp(sp->rs), data, len);
}

static void
deliver_to_client(void *opaque, const void *data, size_t len)
{
  peer_t *p = opaque;
  sim_priv_t *sp = p->priv;
  pthread_mutex_lock(&sp->client_mutex);
  if(p->v != NULL)
    vllp_input(p->v, data, len);
  pthread_mutex_unlock(&sp->client_mutex);
}

static void
client_tx(void *opaque, const void *data, size_t len)
{
  peer_t *p = opaque;
  linksim_send(p->ls, PEER_DIR_C2S, data, len, deliver_to_server, p);
}

static void
server_tx(void *opaque, const void *data, size_t len)
{
  peer_t *p = opaque;
  linksim_send(p->ls, PEER_DIR_S2C, data, len, deliver_to_client, p);
}

static void
server_log(void *opaque, int level, const char *msg)
{
  (void)opaque;
  tst_verbosef("refserver[%d]: %s", level, msg);
}

static vllp_t *
make_client(peer_t *p)
{
  vllp_t *v = vllp_create_client(p->mtu, p->timeout, p->vllp_flags, p,
                                 client_tx, peer_log_cb);
  vllp_start(v);
  return v;
}

static int
sim_restart_client(peer_t *p)
{
  sim_priv_t *sp = p->priv;
  pthread_mutex_lock(&sp->client_mutex);
  vllp_t *old = p->v;
  p->v = NULL;
  pthread_mutex_unlock(&sp->client_mutex);
  vllp_destroy(old);
  vllp_t *nv = make_client(p);
  pthread_mutex_lock(&sp->client_mutex);
  p->v = nv;
  pthread_mutex_unlock(&sp->client_mutex);
  return 0;
}

static int
sim_server_status(peer_t *p, peer_server_status_t *st)
{
  sim_priv_t *sp = p->priv;
  memset(st, 0, sizeof(*st));
  st->connected = vllp_is_connected(refserver_vllp(sp->rs));
  st->user_channels = refserver_open_channels(sp->rs);
  snprintf(st->detail, sizeof(st->detail),
           "refserver connected=%d open_channels=%d",
           st->connected, st->user_channels);
  return 0;
}

static void
sim_destroy(peer_t *p)
{
  sim_priv_t *sp = p->priv;
  pthread_mutex_lock(&sp->client_mutex);
  vllp_t *old = p->v;
  p->v = NULL;
  pthread_mutex_unlock(&sp->client_mutex);
  if(old)
    vllp_destroy(old);
  linksim_drain(p->ls, 1000000);
  refserver_destroy(sp->rs);
  linksim_destroy(p->ls);
  free(sp);
  free(p);
}

peer_t *
peer_sim_create(int mtu, int timeout, uint64_t seed)
{
  peer_t *p = calloc(1, sizeof(*p));
  sim_priv_t *sp = calloc(1, sizeof(*sp));
  pthread_mutex_init(&sp->client_mutex, NULL);
  p->priv = sp;
  peer_init_common(p);
  p->name = mtu > 8 ? "sim-mtu64" : "sim-mtu8";
  p->mtu = mtu;
  p->timeout = timeout;
  p->vllp_flags = mtu > 8 ? VLLP_FDCAN_ADAPTATION : 0;
  p->has_log_service = 0;
  p->reliable_blackout = 1;
  p->server_fragment_size = REFSERVER_FRAGMENT_SIZE;
  p->ls = linksim_create(seed);

  sp->rs = refserver_create(mtu, timeout, p->vllp_flags, p, server_tx,
                            p, server_log);
  p->v = make_client(p);

  p->restart_client = sim_restart_client;
  p->server_status = sim_server_status;
  p->server_console = NULL;
  p->destroy = sim_destroy;
  return p;
}
