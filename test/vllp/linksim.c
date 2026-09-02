#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "linksim.h"
#include "tst.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/queue.h>

typedef struct frame {
  TAILQ_ENTRY(frame) link;
  int64_t deliver_at;
  uint64_t seq;
  linksim_deliver_fn fn;
  void *opaque;
  size_t len;
  uint8_t data[];
} frame_t;

TAILQ_HEAD(frame_queue, frame);

struct linksim {
  pthread_mutex_t mutex;
  pthread_cond_t cond;
  pthread_t tid;
  int run;
  uint64_t seq;
  struct frame_queue q;
  fault_cfg_t cfg[2];
  linksim_stats_t stats[2];
  tst_rng_t rng;
  FILE *trace;
  int trace_fdcan;
  int64_t t0;
};

void
linksim_set_trace(linksim_t *ls, void *fp, int fdcan)
{
  pthread_mutex_lock(&ls->mutex);
  ls->trace = fp;
  ls->trace_fdcan = fdcan;
  ls->t0 = tst_now_us();
  pthread_mutex_unlock(&ls->mutex);
}

static void
trace_frame(linksim_t *ls, int dir, const uint8_t *p, size_t len,
            const char *verdict)
{
  FILE *fp = ls->trace;
  size_t llen = len;
  if(ls->trace_fdcan && len > 8) {
    uint8_t pad = p[len - 1];
    if(pad >= 1 && pad <= len)
      llen = len - pad;
  }
  fprintf(fp, "%10.6f %s %-5s %2zu ", (tst_now_us() - ls->t0) / 1e6,
          dir == 0 ? "C>S" : "S>C", verdict, llen);
  if(llen == 0) {
    fprintf(fp, "empty\n");
    return;
  }
  uint8_t h = p[0];
  int ch = h & 0xf;
  if(ch == 15) {
    if(!(h & 0x10)) {
      fprintf(fp, "SYN ver=%d mtu=%d", llen > 2 ? p[1] : -1, llen > 2 ? p[2] : -1);
    } else {
      fprintf(fp, "ACK S=%d E=%d F=%d flow=0x%04x", !!(h & 0x80), !!(h & 0x40),
              !!(h & 0x20), llen >= 3 ? (p[1] | (p[2] << 8)) : -1);
    }
  } else {
    fprintf(fp, "DAT S=%d E=%d F=%d L=%d ch=%2d", !!(h & 0x80), !!(h & 0x40),
            !!(h & 0x20), !!(h & 0x10), ch);
    if(ch == 14 && llen >= 2)
      fprintf(fp, " cmc op=%d target=%d", p[1] >> 4, p[1] & 0xf);
  }
  fprintf(fp, "\n");
}

static void *
linksim_thread(void *arg)
{
  linksim_t *ls = arg;
  pthread_mutex_lock(&ls->mutex);
  while(ls->run) {
    frame_t *f = TAILQ_FIRST(&ls->q);
    if(f == NULL) {
      pthread_cond_wait(&ls->cond, &ls->mutex);
      continue;
    }
    int64_t now = tst_now_us();
    if(f->deliver_at > now) {
      struct timespec ts = { f->deliver_at / 1000000,
                             (f->deliver_at % 1000000) * 1000 };
      pthread_cond_timedwait(&ls->cond, &ls->mutex, &ts);
      continue;
    }
    TAILQ_REMOVE(&ls->q, f, link);
    pthread_mutex_unlock(&ls->mutex);
    f->fn(f->opaque, f->data, f->len);
    free(f);
    pthread_mutex_lock(&ls->mutex);
  }
  pthread_mutex_unlock(&ls->mutex);
  return NULL;
}

linksim_t *
linksim_create(uint64_t seed)
{
  linksim_t *ls = calloc(1, sizeof(*ls));
  pthread_mutex_init(&ls->mutex, NULL);
  pthread_condattr_t ca;
  pthread_condattr_init(&ca);
  pthread_condattr_setclock(&ca, CLOCK_MONOTONIC);
  pthread_cond_init(&ls->cond, &ca);
  TAILQ_INIT(&ls->q);
  tst_rng_seed(&ls->rng, seed);
  ls->run = 1;
  pthread_create(&ls->tid, NULL, linksim_thread, ls);
  return ls;
}

void
linksim_destroy(linksim_t *ls)
{
  pthread_mutex_lock(&ls->mutex);
  ls->run = 0;
  pthread_cond_signal(&ls->cond);
  pthread_mutex_unlock(&ls->mutex);
  pthread_join(ls->tid, NULL);
  frame_t *f;
  while((f = TAILQ_FIRST(&ls->q)) != NULL) {
    TAILQ_REMOVE(&ls->q, f, link);
    free(f);
  }
  free(ls);
}

void
linksim_set_faults(linksim_t *ls, int dir, const fault_cfg_t *cfg)
{
  pthread_mutex_lock(&ls->mutex);
  if(cfg)
    ls->cfg[dir] = *cfg;
  else
    memset(&ls->cfg[dir], 0, sizeof(fault_cfg_t));
  pthread_mutex_unlock(&ls->mutex);
}

void
linksim_get_faults(linksim_t *ls, int dir, fault_cfg_t *cfg)
{
  pthread_mutex_lock(&ls->mutex);
  *cfg = ls->cfg[dir];
  pthread_mutex_unlock(&ls->mutex);
}

static void
enqueue(linksim_t *ls, int dir, const void *data, size_t len,
        linksim_deliver_fn fn, void *opaque, int corrupt)
{
  const fault_cfg_t *cfg = &ls->cfg[dir];
  frame_t *f = malloc(sizeof(frame_t) + len);
  f->fn = fn;
  f->opaque = opaque;
  f->len = len;
  memcpy(f->data, data, len);
  f->seq = ls->seq++;

  if(corrupt && len > 0) {
    uint32_t bit = tst_rng_u32(&ls->rng) % (len * 8);
    f->data[bit / 8] ^= 1 << (bit & 7);
    ls->stats[dir].corrupted++;
  }

  int64_t delay = cfg->delay_min_us;
  if(cfg->delay_max_us > cfg->delay_min_us)
    delay += tst_rng_u32(&ls->rng) % (cfg->delay_max_us - cfg->delay_min_us);
  f->deliver_at = tst_now_us() + delay;

  // Insert sorted by deliver_at, stable for equal timestamps
  frame_t *it;
  TAILQ_FOREACH_REVERSE(it, &ls->q, frame_queue, link) {
    if(it->deliver_at <= f->deliver_at)
      break;
  }
  if(it == NULL)
    TAILQ_INSERT_HEAD(&ls->q, f, link);
  else
    TAILQ_INSERT_AFTER(&ls->q, it, f, link);
  pthread_cond_signal(&ls->cond);
}

void
linksim_send(linksim_t *ls, int dir, const void *data, size_t len,
             linksim_deliver_fn fn, void *opaque)
{
  pthread_mutex_lock(&ls->mutex);
  const fault_cfg_t *cfg = &ls->cfg[dir];
  linksim_stats_t *st = &ls->stats[dir];
  st->frames++;
  st->bytes += len;

  if(cfg->blackout || (cfg->drop > 0 && tst_rng_unit(&ls->rng) < cfg->drop)) {
    st->dropped++;
    if(ls->trace)
      trace_frame(ls, dir, data, len, "DROP");
    pthread_mutex_unlock(&ls->mutex);
    return;
  }

  int corrupt = cfg->corrupt > 0 && tst_rng_unit(&ls->rng) < cfg->corrupt;
  enqueue(ls, dir, data, len, fn, opaque, corrupt);
  st->delivered++;
  if(ls->trace)
    trace_frame(ls, dir, data, len, corrupt ? "CORR" : "");

  if(cfg->dup > 0 && tst_rng_unit(&ls->rng) < cfg->dup) {
    enqueue(ls, dir, data, len, fn, opaque, 0);
    st->dupped++;
    st->delivered++;
    if(ls->trace)
      trace_frame(ls, dir, data, len, "DUP");
  }
  pthread_mutex_unlock(&ls->mutex);
}

void
linksim_get_stats(linksim_t *ls, int dir, linksim_stats_t *st)
{
  pthread_mutex_lock(&ls->mutex);
  *st = ls->stats[dir];
  pthread_mutex_unlock(&ls->mutex);
}

void
linksim_reset_stats(linksim_t *ls)
{
  pthread_mutex_lock(&ls->mutex);
  memset(ls->stats, 0, sizeof(ls->stats));
  pthread_mutex_unlock(&ls->mutex);
}

int
linksim_drain(linksim_t *ls, int64_t timeout_us)
{
  int64_t deadline = tst_now_us() + timeout_us;
  while(1) {
    pthread_mutex_lock(&ls->mutex);
    int empty = TAILQ_FIRST(&ls->q) == NULL;
    pthread_mutex_unlock(&ls->mutex);
    if(empty)
      return 0;
    if(tst_now_us() > deadline)
      return -1;
    tst_sleep_us(1000);
  }
}
