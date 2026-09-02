#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "dsig_vllp.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>

// Over a loopback-enabled transport (UDP multicast -- see dsig_udp.h),
// a client sees its own transmitted frame echo straight back to itself
// microseconds later, with no real round trip to the device involved.
// VLLP has no way to tell that apart from a genuine (if suspiciously
// fast) reply and gets stuck waiting for one that already "arrived" --
// observed hanging an OTA transfer indefinitely.
//
// The filter is opt-in (VLLP_FILTER_SELF_ECHO) because it is NOT free on
// non-echoing transports: VLLP ACK frames carry only the SE bits, the
// flow word and a CRC over a shared IV, so the peer's ACK is frequently
// byte-identical to the ACK we sent a moment earlier (whenever both
// sides' S bits agree and both flow words are 0xffff). With the filter
// always on, the PMD's flow-resume and keepalive ACKs over CAN were
// dropped as "echo" and OTA transfers stalled into timeout.
#define DV_RECENT_FRAMES 8
#define DV_RECENT_WINDOW_US 200000

struct dv_recent {
  uint32_t hash;
  uint32_t len;
  int64_t t_us;
};

struct dsig_vllp {
  dsig_t *bus;
  uint32_t txid;
  vllp_t *vllp;
  dsig_sub_t *sub;

  int filter_echo; // VLLP_FILTER_SELF_ECHO requested at create
  struct dv_recent recent[DV_RECENT_FRAMES];
  int recent_idx;

  void *user_opaque;
  void (*user_log)(void *opaque, int level, const char *msg);
  open_channel_result_t (*user_open_channel)(void *opaque, const char *name,
                                             vllp_channel_t *vc);
};

static uint32_t
dv_fnv1a(const void *data, size_t len)
{
  const uint8_t *p = data;
  uint32_t h = 2166136261u;
  for(size_t i = 0; i < len; i++) {
    h ^= p[i];
    h *= 16777619u;
  }
  return h;
}

static int64_t
dv_now_us(void)
{
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (int64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}

static void
dv_remember_sent(dsig_vllp_t *dv, const void *data, size_t len)
{
  struct dv_recent *r = &dv->recent[dv->recent_idx];
  r->hash = dv_fnv1a(data, len);
  r->len = (uint32_t)len;
  r->t_us = dv_now_us();
  dv->recent_idx = (dv->recent_idx + 1) % DV_RECENT_FRAMES;
}

static int
dv_was_recently_sent(dsig_vllp_t *dv, const void *data, size_t len)
{
  uint32_t h = dv_fnv1a(data, len);
  int64_t now = dv_now_us();
  for(int i = 0; i < DV_RECENT_FRAMES; i++) {
    struct dv_recent *r = &dv->recent[i];
    if(r->len == (uint32_t)len && r->hash == h &&
       (now - r->t_us) < DV_RECENT_WINDOW_US)
      return 1;
  }
  return 0;
}

static void
dv_tx(void *opaque, const void *data, size_t len)
{
  dsig_vllp_t *dv = opaque;
  if(dv->filter_echo)
    dv_remember_sent(dv, data, len);
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
  if(dv->filter_echo && dv_was_recently_sent(dv, data, len))
    return; // self-echo (UDP multicast loopback), not a real reply
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
  dv->filter_echo = !!(vllp_flags & VLLP_FILTER_SELF_ECHO);
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
  dv->filter_echo = !!(vllp_flags & VLLP_FILTER_SELF_ECHO);
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
