#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "refserver.h"
#include "tst.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <sys/queue.h>

typedef struct rs_chan {
  LIST_ENTRY(rs_chan) link;
  refserver_t *rs;
  vllp_channel_t *vc;
  char name[32];
  pthread_t reader;
  pthread_t writer;
  int has_writer;
  volatile int closed;
} rs_chan_t;

struct refserver {
  vllp_t *v;
  pthread_mutex_t mutex;
  LIST_HEAD(, rs_chan) chans;
  LIST_HEAD(, rs_chan) done;   /* threads finished, waiting for join */
  int open_channels;

  void (*tx)(void *opaque, const void *data, size_t len);
  void *tx_opaque;
  void (*log)(void *opaque, int level, const char *msg);
  void *log_opaque;
};

static void
rs_tx(void *opaque, const void *data, size_t len)
{
  refserver_t *rs = opaque;
  rs->tx(rs->tx_opaque, data, len);
}

static void
rs_log(void *opaque, int level, const char *msg)
{
  refserver_t *rs = opaque;
  if(rs->log)
    rs->log(rs->log_opaque, level, msg);
}

static void *
chargen_writer(void *arg)
{
  rs_chan_t *c = arg;
  uint8_t buf[REFSERVER_FRAGMENT_SIZE];
  for(size_t i = 0; i < sizeof(buf); i++)
    buf[i] = (uint8_t)i;

  while(!c->closed) {
    if(vllp_channel_tx_pending(c->vc) < 4) {
      vllp_channel_send(c->vc, buf, sizeof(buf));
    } else {
      tst_sleep_us(200);
    }
  }
  return NULL;
}

static void *
chan_reader(void *arg)
{
  rs_chan_t *c = arg;
  refserver_t *rs = c->rs;
  const int echo = !strcmp(c->name, "echo");

  while(1) {
    void *data;
    size_t len;
    int err = vllp_channel_read(c->vc, &data, &len, -1);
    if(err || data == NULL) {
      /* err == 0 with data == NULL is a clean EOF */
      tst_verbosef("refserver: %s channel %d: read -> %s", c->name,
                   vllp_channel_id(c->vc), err ? vllp_strerror(err) : "EOF");
      break;
    }
    if(echo)
      vllp_channel_send(c->vc, data, len);
    free(data);
  }
  c->closed = 1;
  if(c->has_writer)
    pthread_join(c->writer, NULL);

  // Peer closed (or link went down). Release our side.
  vllp_channel_close(c->vc, 0, 0);

  pthread_mutex_lock(&rs->mutex);
  LIST_REMOVE(c, link);
  LIST_INSERT_HEAD(&rs->done, c, link);
  rs->open_channels--;
  pthread_mutex_unlock(&rs->mutex);
  return NULL;
}

static open_channel_result_t
rs_open(void *opaque, const char *name, vllp_channel_t *vc)
{
  refserver_t *rs = opaque;
  open_channel_result_t r = {};

  if(strcmp(name, "echo") && strcmp(name, "chargen") &&
     strcmp(name, "discard")) {
    r.error = VLLP_ERR_NOT_FOUND;
    return r;
  }

  rs_chan_t *c = calloc(1, sizeof(*c));
  c->rs = rs;
  c->vc = vc;
  snprintf(c->name, sizeof(c->name), "%s", name);

  pthread_mutex_lock(&rs->mutex);
  LIST_INSERT_HEAD(&rs->chans, c, link);
  rs->open_channels++;
  pthread_mutex_unlock(&rs->mutex);

  if(!strcmp(name, "chargen")) {
    c->has_writer = 1;
    pthread_create(&c->writer, NULL, chargen_writer, c);
  }
  tst_verbosef("refserver: open %s on channel %d", name, vllp_channel_id(vc));
  pthread_create(&c->reader, NULL, chan_reader, c);
  return r; /* rx/eof NULL: we consume via vllp_channel_read() */
}

refserver_t *
refserver_create(int mtu, int timeout, uint32_t flags,
                 void *tx_opaque,
                 void (*tx)(void *opaque, const void *data, size_t len),
                 void *log_opaque,
                 void (*log)(void *opaque, int level, const char *msg))
{
  refserver_t *rs = calloc(1, sizeof(*rs));
  pthread_mutex_init(&rs->mutex, NULL);
  LIST_INIT(&rs->chans);
  LIST_INIT(&rs->done);
  rs->tx = tx;
  rs->tx_opaque = tx_opaque;
  rs->log = log;
  rs->log_opaque = log_opaque;

  /* vllp_create_server hands the same opaque to tx, log and open, so
     route through trampolines */
  rs->v = vllp_create_server(mtu, timeout, flags, rs, rs_tx, rs_log, rs_open);
  if(rs->v == NULL) {
    free(rs);
    return NULL;
  }
  vllp_start(rs->v);
  return rs;
}

vllp_t *
refserver_vllp(refserver_t *rs)
{
  return rs->v;
}

int
refserver_open_channels(refserver_t *rs)
{
  pthread_mutex_lock(&rs->mutex);
  int n = rs->open_channels;
  pthread_mutex_unlock(&rs->mutex);
  return n;
}

void
refserver_destroy(refserver_t *rs)
{
  int64_t deadline = tst_now_us() + 5000000;
  while(refserver_open_channels(rs) > 0 && tst_now_us() < deadline)
    tst_sleep_us(10000);
  if(refserver_open_channels(rs) > 0)
    tst_logf("refserver: %d channels still open at destroy",
             refserver_open_channels(rs));

  rs_chan_t *c;
  pthread_mutex_lock(&rs->mutex);
  while((c = LIST_FIRST(&rs->done)) != NULL) {
    LIST_REMOVE(c, link);
    pthread_mutex_unlock(&rs->mutex);
    pthread_join(c->reader, NULL);
    free(c);
    pthread_mutex_lock(&rs->mutex);
  }
  pthread_mutex_unlock(&rs->mutex);

  vllp_destroy(rs->v);
  free(rs);
}
