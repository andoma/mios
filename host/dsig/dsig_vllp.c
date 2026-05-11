#define _GNU_SOURCE
#include "dsig_vllp.h"

#include <stdlib.h>

struct dsig_vllp {
  dsig_t *bus;
  uint32_t txid;
  vllp_t *vllp;
  dsig_sub_t *sub;

  void *user_opaque;
  void (*user_log)(void *opaque, int level, const char *msg);
  open_channel_result_t (*user_open_channel)(void *opaque, const char *name,
                                             vllp_channel_t *vc);
};

static void
dv_tx(void *opaque, const void *data, size_t len)
{
  dsig_vllp_t *dv = opaque;
  dsig_send(dv->bus, dv->txid, data, len);
}

static void
dv_log_thunk(void *opaque, int level, const char *msg)
{
  dsig_vllp_t *dv = opaque;
  if(dv->user_log != NULL)
    dv->user_log(dv->user_opaque, level, msg);
}

static open_channel_result_t
dv_open_channel_thunk(void *opaque, const char *name, vllp_channel_t *vc)
{
  dsig_vllp_t *dv = opaque;
  return dv->user_open_channel(dv->user_opaque, name, vc);
}

static void
dv_rx(void *opaque, uint32_t signal, const void *data, size_t len)
{
  dsig_vllp_t *dv = opaque;
  (void)signal;
  if(data == NULL || len == 0)
    return;  // ttl timeout sentinel — ignored
  vllp_input(dv->vllp, data, len);
}

static dsig_vllp_t *
finish_setup(dsig_vllp_t *dv, uint32_t rxid)
{
  if(dv->vllp == NULL) {
    free(dv);
    return NULL;
  }
  dv->sub = dsig_sub(dv->bus, rxid, 0xffffffff, 0, dv_rx, dv);
  if(dv->sub == NULL) {
    vllp_destroy(dv->vllp);
    free(dv);
    return NULL;
  }
  vllp_start(dv->vllp);
  return dv;
}

dsig_vllp_t *
dsig_vllp_client_create(dsig_t *bus, uint32_t txid, uint32_t rxid,
                        int mtu, int timeout, uint32_t vllp_flags,
                        void *log_opaque,
                        void (*log)(void *opaque, int syslog_level,
                                    const char *msg))
{
  dsig_vllp_t *dv = calloc(1, sizeof(*dv));
  if(dv == NULL)
    return NULL;
  dv->bus = bus;
  dv->txid = txid;
  dv->user_opaque = log_opaque;
  dv->user_log = log;
  dv->vllp = vllp_create_client(mtu, timeout, vllp_flags, dv,
                                dv_tx, dv_log_thunk);
  return finish_setup(dv, rxid);
}

dsig_vllp_t *
dsig_vllp_server_create(dsig_t *bus, uint32_t txid, uint32_t rxid,
                        int mtu, int timeout, uint32_t vllp_flags,
                        void *opaque,
                        void (*log)(void *opaque, int syslog_level,
                                    const char *msg),
                        open_channel_result_t (*open_channel)(
                            void *opaque, const char *name,
                            vllp_channel_t *vc))
{
  dsig_vllp_t *dv = calloc(1, sizeof(*dv));
  if(dv == NULL)
    return NULL;
  dv->bus = bus;
  dv->txid = txid;
  dv->user_opaque = opaque;
  dv->user_log = log;
  dv->user_open_channel = open_channel;
  dv->vllp = vllp_create_server(mtu, timeout, vllp_flags, dv,
                                dv_tx, dv_log_thunk, dv_open_channel_thunk);
  return finish_setup(dv, rxid);
}

vllp_t *
dsig_vllp_get_vllp(dsig_vllp_t *dv)
{
  return dv->vllp;
}

void
dsig_vllp_destroy(dsig_vllp_t *dv)
{
  if(dv == NULL)
    return;
  dsig_unsub(dv->sub);
  vllp_destroy(dv->vllp);
  free(dv);
}
