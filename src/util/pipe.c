#include <mios/pipe.h>

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

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
} pipe_t;


static void
pipe_setup_poll(pollset_t *ps, pipe_stream_t *in, pipe_stream_t *out)
{
  if(in->used != PIPE_BUF_SIZE) {
    ps->obj = in->s;
    ps->type = POLL_STREAM_READ;
  } else {
    ps->obj = out->s;
    ps->type = POLL_STREAM_WRITE;
  }
}



static void
pipe_handle(pipe_stream_t *in, pipe_stream_t *out)
{
  if(in->used != PIPE_BUF_SIZE) {
    in->used += in->s->read(in->s, in->buf + in->used,
                            PIPE_BUF_SIZE - in->used, 0);
  }

  ssize_t r = out->s->write(out->s, in->buf, in->used, STREAM_WRITE_NO_WAIT);
  if(r >= 0) {
    if(r == in->used) {
      in->used = 0;
      return;
    }
    memmove(in->buf, in->buf + r, in->used - r);
    in->used -= r;
  }
}

__attribute__((noreturn))
static void *
pipe_thread(void *arg)
{
  pipe_t *p = arg;

  while(1) {
    pipe_setup_poll(&p->ps[0], &p->a, &p->b);
    pipe_setup_poll(&p->ps[1], &p->b, &p->a);

    int which = poll(p->ps, 2, NULL, INT64_MAX);

    if(which == 0)
      pipe_handle(&p->a, &p->b);
    else
      pipe_handle(&p->b, &p->a);
  }
}


void
pipe_bidir(stream_t *a, stream_t *b, const char *name)
{
  pipe_t *p = malloc(sizeof(pipe_t));
  p->a.s = a;
  p->b.s = b;
  thread_create(pipe_thread, p, 256, name, TASK_DETACHED, 4);
}
