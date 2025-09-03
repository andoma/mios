#include <mios/pipe.h>
#include <mios/stream.h>
#include <mios/task.h>
#include <mios/atomic.h>

#include <sys/param.h>

#include <string.h>
#include <stdlib.h>
#include <assert.h>

#include "irq.h"

typedef struct pipe_stream {
  stream_t s;
  mutex_t mutex;
  cond_t cond;
  const uint8_t *write_ptr;
  size_t write_size;
  int eof;
} pipe_stream_t;

typedef struct pipe {
  pipe_stream_t a;
  pipe_stream_t b;
  atomic_t closecount;
} pipe_t;


static ssize_t
pipe_read(struct stream *s, void *buf,
          size_t size, size_t required)
{
  pipe_stream_t *ps = (pipe_stream_t *)s;

  mutex_lock(&ps->mutex);

  size_t i = 0;
  while(i != size) {
    while(ps->write_size == 0) {
      if(i >= required) {
        mutex_unlock(&ps->mutex);
        return i;
      }
      if(ps->eof) {
        mutex_unlock(&ps->mutex);
        return size;
      }
      cond_wait(&ps->cond, &ps->mutex);
    }

    size_t to_copy = MIN(size - i, ps->write_size);
    assert(to_copy > 0);
    if(buf) {
      memcpy(buf, ps->write_ptr, to_copy);
      buf += to_copy;
    }

    i += to_copy;

    ps->write_size -= to_copy;
    ps->write_ptr += to_copy;
    cond_signal(&ps->cond);
  }

  mutex_unlock(&ps->mutex);
  return size;
}

static ssize_t
pipe_drop(struct stream *st, size_t bytes)
{
  return pipe_read(st, NULL, bytes, bytes);
}

static ssize_t
pipe_write(pipe_stream_t *ps, const void *buf,
          size_t size, int flags)
{
  if(!can_sleep())
    return size;

  mutex_lock(&ps->mutex);

  // Protect from racing when having multiple writers
  while(ps->write_size) {
    if(ps->eof) {
      mutex_unlock(&ps->mutex);
      return 0;
    }
    cond_wait(&ps->cond, &ps->mutex);
  }

  ps->write_ptr = buf;
  ps->write_size = size;

  cond_broadcast(&ps->cond);

  while(ps->write_size) {
    if(ps->eof) {
      mutex_unlock(&ps->mutex);
      return 0;
    }
    cond_wait(&ps->cond, &ps->mutex);
  }

  mutex_unlock(&ps->mutex);
  return size;
}

static ssize_t
pipe_write_a(struct stream *s, const void *buf,
             size_t size, int flags)
{
  pipe_stream_t *ps = (pipe_stream_t *)s;
  return pipe_write(ps + 1, buf, size, flags);

}

static ssize_t
pipe_write_b(struct stream *s, const void *buf,
             size_t size, int flags)
{
  pipe_stream_t *ps = (pipe_stream_t *)s;
  return pipe_write(ps - 1, buf, size, flags);
}

static void
pipe_stream_close(pipe_stream_t *ps)
{
  mutex_lock(&ps->mutex);
  ps->eof = 1;
  cond_broadcast(&ps->cond);
  mutex_unlock(&ps->mutex);
}

static void
pipe_close(pipe_t *p)
{
  if(atomic_add_and_fetch(&p->closecount, 1) == 2)
    free(p);
}


static void
pipe_close_a(struct stream *s)
{
  pipe_stream_t *ps = (pipe_stream_t *)s;
  pipe_stream_close(ps);
  pipe_close((pipe_t *)ps);
}

static void
pipe_close_b(struct stream *s)
{
  pipe_stream_t *ps = (pipe_stream_t *)s;
  pipe_stream_close(ps);
  ps--;
  pipe_close((pipe_t *)ps);
}


static task_waitable_t *
pipe_poll(struct stream *s, poll_type_t type)
{
  pipe_stream_t *ps = (pipe_stream_t *)s;

  irq_forbid(IRQ_LEVEL_SWITCH);

  if(type == POLL_STREAM_READ) {
    if(ps->write_size)
      return NULL;
    return &ps->cond;

  } else {
    return NULL;
  }
}


static const stream_vtable_t pipe_vtable_a = {
  .read = pipe_read,
  .write = pipe_write_a,
  .close = pipe_close_a,
  .poll = pipe_poll,
  .drop = pipe_drop,
};

static const stream_vtable_t pipe_vtable_b = {
  .read = pipe_read,
  .write = pipe_write_b,
  .close = pipe_close_b,
  .poll = pipe_poll,
  .drop = pipe_drop,
};


static void
pipe_stream_init(pipe_stream_t *ps)
{
  mutex_init(&ps->mutex, "pipe");
  cond_init(&ps->cond, "pipe");
}

void
pipe(struct stream **a, struct stream **b)
{
  pipe_t *p = calloc(1, sizeof(pipe_t));
  p->a.s.vtable = &pipe_vtable_a;
  p->b.s.vtable = &pipe_vtable_b;
  pipe_stream_init(&p->a);
  pipe_stream_init(&p->b);
  *a = &p->a.s;
  *b = &p->b.s;
}
