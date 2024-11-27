#include <mios/pipe.h>

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <stdio.h>

#include <mios/stream.h>
#include <mios/task.h>


#define PIPE_BUF_SIZE 16

typedef struct {
  struct stream *s;
  uint8_t buf[PIPE_BUF_SIZE];
  size_t used;
} pipe_stream_t;

typedef struct {
  pipe_stream_t a;
  pipe_stream_t b;
  pollset_t ps[2];
  uint16_t escape_character;
} pipe_t;


static void
pipe_setup_poll(pollset_t *ps, pipe_stream_t *in, pipe_stream_t *out)
{
  if(in->used == 0) {
    ps->obj = in->s;
    ps->type = POLL_STREAM_READ;
  } else {
    ps->obj = out->s;
    ps->type = POLL_STREAM_WRITE;
  }
}

static error_t
pipe_handle(pipe_stream_t *in, pipe_stream_t *out, pipe_t *p)
{
  if(in->used == 0) {
    in->used += stream_read(in->s, in->buf + in->used,
                            PIPE_BUF_SIZE - in->used, 0);
  }

  if(p->escape_character < 256) {
    for(size_t i = 0; i < in->used; i++) {
      if(in->buf[i] == p->escape_character)
        return ERR_INTERRUPTED;
    }
  }

  ssize_t r = stream_write(out->s, in->buf, in->used, STREAM_WRITE_NO_WAIT);
  if(r < 0)
    return r;

  if(r == in->used) {
    in->used = 0;
  } else {
    memmove(in->buf, in->buf + r, in->used - r);
    in->used -= r;
  }
  return 0;
}


__attribute__((noinline))
static error_t
pipe_loop(pipe_t *p)
{
  while(1) {
    pipe_setup_poll(&p->ps[0], &p->a, &p->b);
    pipe_setup_poll(&p->ps[1], &p->b, &p->a);

    int which = poll(p->ps, 2, NULL, INT64_MAX);

    error_t err;
    if(which == 0)
      err = pipe_handle(&p->a, &p->b, p);
    else
      err = pipe_handle(&p->b, &p->a, p);
    if(err < 0)
      return err;
  }
}


__attribute__((noreturn))
static void *
pipe_thread(void *arg)
{
  pipe_loop(arg);
  thread_exit(NULL);
}

error_t
pipe_bidir(stream_t *a, stream_t *b, const char *name, int flags,
           int escape_character)
{
  pipe_t *p = malloc(sizeof(pipe_t));
  p->a.s = a;
  p->b.s = b;
  p->a.used = 0;
  p->b.used = 0;
  p->escape_character = escape_character;
  if(flags & PIPE_BACKGROUND) {
    thread_create(pipe_thread, p, 256, name, TASK_DETACHED | TASK_NO_FPU, 4);
    return 0;
  } else {
    return pipe_loop(p);
  }
}
