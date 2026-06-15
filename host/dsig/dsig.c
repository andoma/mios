#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "dsig.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/queue.h>
#include <time.h>
#include <unistd.h>

#define EXPIRE_NEVER INT64_MAX

TAILQ_HEAD(dsig_sub_list, dsig_sub);
TAILQ_HEAD(dsig_emitter_list, dsig_emitter);

struct dsig_sub {
  TAILQ_ENTRY(dsig_sub) link;
  dsig_t *bus;
  uint32_t signal;
  uint32_t mask;
  int64_t ttl_us;        // 0 == no timeout
  int64_t expire_us;     // EXPIRE_NEVER when disarmed
  dsig_rx_cb cb;
  void *opaque;
  int dead;
};

struct dsig_emitter {
  TAILQ_ENTRY(dsig_emitter) link;
  dsig_t *bus;
  uint32_t signal;
  int64_t refresh_us;    // 0 == no auto-repeat
  int64_t expire_us;     // EXPIRE_NEVER when disabled
  uint8_t *data;
  size_t len;
};

struct dsig {
  pthread_t tid;
  pthread_mutex_t mtx;
  int wakeup_pipe[2];
  int stop;

  struct dsig_sub_list subs;
  struct dsig_emitter_list emitters;

  dsig_tx_fn tx;
  void *tx_opaque;
};

static int64_t
monotonic_us(void)
{
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (int64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}

static void
wakeup(dsig_t *d)
{
  uint8_t c = 0;
  ssize_t r = write(d->wakeup_pipe[1], &c, 1);
  (void)r;
}

static void
drain_pipe(int fd)
{
  uint8_t buf[64];
  while(read(fd, buf, sizeof(buf)) > 0) {
  }
}

static void
emit_locked(dsig_t *d, dsig_emitter_t *e)
{
  if(d->tx != NULL)
    d->tx(d->tx_opaque, e->signal, e->data, e->len);
}

static void *
bus_thread(void *arg)
{
  dsig_t *d = arg;

  struct pollfd pfd = { .fd = d->wakeup_pipe[0], .events = POLLIN };

  while(1) {
    pthread_mutex_lock(&d->mtx);

    if(d->stop) {
      pthread_mutex_unlock(&d->mtx);
      break;
    }

    int64_t now = monotonic_us();
    int64_t next = EXPIRE_NEVER;

    // Fire any due emitters
    dsig_emitter_t *e;
    TAILQ_FOREACH(e, &d->emitters, link) {
      if(e->expire_us == EXPIRE_NEVER)
        continue;
      if(e->expire_us <= now) {
        emit_locked(d, e);
        e->expire_us = e->refresh_us ? now + e->refresh_us : EXPIRE_NEVER;
      }
      if(e->expire_us < next)
        next = e->expire_us;
    }

    // Collect any expired subscribers; fire them after unlock.
    struct {
      dsig_rx_cb cb;
      void *opaque;
    } fire[16];
    int nfire = 0;
    dsig_rx_cb *fire_overflow_cb = NULL;
    void **fire_overflow_op = NULL;
    int fire_overflow_n = 0;

    dsig_sub_t *s;
    TAILQ_FOREACH(s, &d->subs, link) {
      if(s->dead)
        continue;
      if(s->expire_us == EXPIRE_NEVER)
        continue;
      if(s->expire_us <= now) {
        s->expire_us = EXPIRE_NEVER;
        if(nfire < (int)(sizeof(fire) / sizeof(fire[0]))) {
          fire[nfire].cb = s->cb;
          fire[nfire].opaque = s->opaque;
          nfire++;
        } else {
          fire_overflow_cb = realloc(fire_overflow_cb,
                                     sizeof(*fire_overflow_cb) *
                                     (fire_overflow_n + 1));
          fire_overflow_op = realloc(fire_overflow_op,
                                     sizeof(*fire_overflow_op) *
                                     (fire_overflow_n + 1));
          fire_overflow_cb[fire_overflow_n] = s->cb;
          fire_overflow_op[fire_overflow_n] = s->opaque;
          fire_overflow_n++;
        }
      } else if(s->expire_us < next) {
        next = s->expire_us;
      }
    }

    pthread_mutex_unlock(&d->mtx);

    for(int i = 0; i < nfire; i++)
      fire[i].cb(fire[i].opaque, 0, NULL, 0);
    for(int i = 0; i < fire_overflow_n; i++)
      fire_overflow_cb[i](fire_overflow_op[i], 0, NULL, 0);
    free(fire_overflow_cb);
    free(fire_overflow_op);

    int timeout_ms;
    if(next == EXPIRE_NEVER) {
      timeout_ms = -1;
    } else {
      int64_t delta_us = next - monotonic_us();
      if(delta_us < 0)
        delta_us = 0;
      int64_t ms = (delta_us + 999) / 1000;
      timeout_ms = ms > INT32_MAX ? INT32_MAX : (int)ms;
    }

    poll(&pfd, 1, timeout_ms);
    if(pfd.revents & POLLIN)
      drain_pipe(d->wakeup_pipe[0]);
  }
  return NULL;
}

dsig_t *
dsig_create(dsig_tx_fn tx, void *tx_opaque)
{
  dsig_t *d = calloc(1, sizeof(*d));
  if(d == NULL)
    return NULL;

  if(pipe2(d->wakeup_pipe, O_CLOEXEC | O_NONBLOCK)) {
    free(d);
    return NULL;
  }

  pthread_mutex_init(&d->mtx, NULL);
  TAILQ_INIT(&d->subs);
  TAILQ_INIT(&d->emitters);
  d->tx = tx;
  d->tx_opaque = tx_opaque;

  if(pthread_create(&d->tid, NULL, bus_thread, d)) {
    pthread_mutex_destroy(&d->mtx);
    close(d->wakeup_pipe[0]);
    close(d->wakeup_pipe[1]);
    free(d);
    return NULL;
  }
  return d;
}

void
dsig_destroy(dsig_t *d)
{
  pthread_mutex_lock(&d->mtx);
  d->stop = 1;
  pthread_mutex_unlock(&d->mtx);
  wakeup(d);
  pthread_join(d->tid, NULL);

  dsig_sub_t *s;
  while((s = TAILQ_FIRST(&d->subs)) != NULL) {
    TAILQ_REMOVE(&d->subs, s, link);
    free(s);
  }
  dsig_emitter_t *e;
  while((e = TAILQ_FIRST(&d->emitters)) != NULL) {
    TAILQ_REMOVE(&d->emitters, e, link);
    free(e->data);
    free(e);
  }

  pthread_mutex_destroy(&d->mtx);
  close(d->wakeup_pipe[0]);
  close(d->wakeup_pipe[1]);
  free(d);
}

int
dsig_send(dsig_t *d, uint32_t signal, const void *data, size_t len)
{
  if(d->tx == NULL)
    return -1;
  d->tx(d->tx_opaque, signal, data, len);
  return 0;
}

void
dsig_input(dsig_t *d, uint32_t signal, const void *data, size_t len)
{
  pthread_mutex_lock(&d->mtx);
  int64_t now = monotonic_us();

  // Snapshot matching callbacks under the lock; rearm their TTL.
  struct fire_entry {
    dsig_rx_cb cb;
    void *opaque;
  };
  struct fire_entry stack_fire[16];
  struct fire_entry *fire = stack_fire;
  int cap = sizeof(stack_fire) / sizeof(stack_fire[0]);
  int n = 0;
  int heap_alloc = 0;

  dsig_sub_t *s;
  TAILQ_FOREACH(s, &d->subs, link) {
    if(s->dead)
      continue;
    if((signal & s->mask) != s->signal)
      continue;
    if(s->ttl_us)
      s->expire_us = now + s->ttl_us;
    if(n == cap) {
      cap *= 2;
      struct fire_entry *grown;
      if(heap_alloc) {
        grown = realloc(fire, sizeof(*grown) * cap);
      } else {
        grown = malloc(sizeof(*grown) * cap);
        if(grown != NULL)
          memcpy(grown, stack_fire, sizeof(stack_fire));
        heap_alloc = 1;
      }
      if(grown == NULL) {
        // Best-effort: drop the rest.
        break;
      }
      fire = grown;
    }
    fire[n].cb = s->cb;
    fire[n].opaque = s->opaque;
    n++;
  }
  pthread_mutex_unlock(&d->mtx);

  // If we rearmed TTLs, the next wakeup might want to recompute the
  // sleep budget. Cheap to nudge; bus_thread re-reads under the lock.
  wakeup(d);

  for(int i = 0; i < n; i++)
    fire[i].cb(fire[i].opaque, signal, data, len);

  if(heap_alloc)
    free(fire);
}

dsig_sub_t *
dsig_sub(dsig_t *d, uint32_t signal, uint32_t mask, int ttl_ms,
         dsig_rx_cb cb, void *opaque)
{
  dsig_sub_t *s = calloc(1, sizeof(*s));
  if(s == NULL)
    return NULL;
  s->bus = d;
  s->signal = signal;
  s->mask = mask;
  s->ttl_us = (int64_t)ttl_ms * 1000;
  s->expire_us = ttl_ms ? monotonic_us() + s->ttl_us : EXPIRE_NEVER;
  s->cb = cb;
  s->opaque = opaque;

  pthread_mutex_lock(&d->mtx);
  TAILQ_INSERT_TAIL(&d->subs, s, link);
  pthread_mutex_unlock(&d->mtx);
  wakeup(d);
  return s;
}

void
dsig_unsub(dsig_sub_t *s)
{
  if(s == NULL)
    return;
  dsig_t *d = s->bus;
  pthread_mutex_lock(&d->mtx);
  TAILQ_REMOVE(&d->subs, s, link);
  pthread_mutex_unlock(&d->mtx);
  free(s);
}

dsig_emitter_t *
dsig_emitter_create(dsig_t *d, uint32_t signal, int refresh_ms)
{
  dsig_emitter_t *e = calloc(1, sizeof(*e));
  if(e == NULL)
    return NULL;
  e->bus = d;
  e->signal = signal;
  e->refresh_us = (int64_t)refresh_ms * 1000;
  e->expire_us = EXPIRE_NEVER;

  pthread_mutex_lock(&d->mtx);
  TAILQ_INSERT_TAIL(&d->emitters, e, link);
  pthread_mutex_unlock(&d->mtx);
  return e;
}

void
dsig_emitter_update(dsig_emitter_t *e, const void *data, size_t len)
{
  dsig_t *d = e->bus;

  uint8_t *new_data = NULL;
  if(len) {
    new_data = malloc(len);
    if(new_data == NULL)
      return;
    memcpy(new_data, data, len);
  }

  pthread_mutex_lock(&d->mtx);
  free(e->data);
  e->data = new_data;
  e->len = len;
  emit_locked(d, e);
  e->expire_us = e->refresh_us ? monotonic_us() + e->refresh_us : EXPIRE_NEVER;
  pthread_mutex_unlock(&d->mtx);
  wakeup(d);
}

void
dsig_emitter_set_refresh(dsig_emitter_t *e, int refresh_ms)
{
  dsig_t *d = e->bus;
  pthread_mutex_lock(&d->mtx);
  int64_t old = e->refresh_us;
  e->refresh_us = (int64_t)refresh_ms * 1000;
  if(e->refresh_us == 0) {
    e->expire_us = EXPIRE_NEVER;
  } else if(old == 0) {
    // Just enabled: emit once now and start the cadence.
    emit_locked(d, e);
    e->expire_us = monotonic_us() + e->refresh_us;
  } else {
    e->expire_us = monotonic_us() + e->refresh_us;
  }
  pthread_mutex_unlock(&d->mtx);
  if(refresh_ms)
    wakeup(d);
}

void
dsig_emitter_destroy(dsig_emitter_t *e)
{
  if(e == NULL)
    return;
  dsig_t *d = e->bus;
  pthread_mutex_lock(&d->mtx);
  TAILQ_REMOVE(&d->emitters, e, link);
  pthread_mutex_unlock(&d->mtx);
  free(e->data);
  free(e);
}
