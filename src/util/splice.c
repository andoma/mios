#include <mios/splice.h>

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <stdio.h>

#include <mios/stream.h>
#include <mios/task.h>


#define SPLICE_BUF_SIZE 16

typedef struct {
  struct stream *s;
  uint8_t buf[SPLICE_BUF_SIZE];
  size_t used;
} splice_stream_t;

typedef struct {
  splice_stream_t a;
  splice_stream_t b;
  pollset_t ps[2];
  uint16_t escape_character;
} splice_t;


static void
splice_setup_poll(pollset_t *ps, splice_stream_t *in, splice_stream_t *out)
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
splice_handle(splice_stream_t *in, splice_stream_t *out, splice_t *p)
{
  if(in->used == 0) {
    in->used += stream_read(in->s, in->buf + in->used,
                            SPLICE_BUF_SIZE - in->used, 0);
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
splice_loop(splice_t *p)
{
  while(1) {
    splice_setup_poll(&p->ps[0], &p->a, &p->b);
    splice_setup_poll(&p->ps[1], &p->b, &p->a);

    int which = poll(p->ps, 2, NULL, INT64_MAX);

    error_t err;
    if(which == 0)
      err = splice_handle(&p->a, &p->b, p);
    else
      err = splice_handle(&p->b, &p->a, p);
    if(err < 0)
      return err;
  }
}


__attribute__((noreturn))
static void *
splice_thread(void *arg)
{
  splice_loop(arg);
  thread_exit(NULL);
}

error_t
splice_bidir(stream_t *a, stream_t *b, const char *name, int flags,
           int escape_character)
{
  splice_t *p = malloc(sizeof(splice_t));
  p->a.s = a;
  p->b.s = b;
  p->a.used = 0;
  p->b.used = 0;
  p->escape_character = escape_character;
  if(flags & SPLICE_BACKGROUND) {
    thread_create(splice_thread, p, 256, name, TASK_DETACHED | TASK_NO_FPU, 4);
    return 0;
  } else {
    return splice_loop(p);
  }
}
