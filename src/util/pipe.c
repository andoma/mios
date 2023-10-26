#include <mios/pipe.h>

#include <stdint.h>
#include <stdlib.h>

#include <mios/stream.h>
#include <mios/task.h>


typedef struct {
  struct stream *from;
  struct stream *to;
} pipe_t;


__attribute__((noreturn))
static void *
pipe_thread(void *arg)
{
  uint8_t buf[16];
  pipe_t *p = arg;
  struct stream *from = p->from;
  struct stream *to = p->to;
  free(p);
  while(1) {
    int r = from->read(from, buf, sizeof(buf), STREAM_READ_WAIT_ONE);
    to->write(to, buf, r);
  }
}


void
pipe_bidir(stream_t *a, stream_t *b)
{
  pipe_t *ab = malloc(sizeof(pipe_t));
  ab->from = a;
  ab->to = b;
  thread_create(pipe_thread, ab, 256, "pipe", TASK_DETACHED, 4);

  pipe_t *ba = malloc(sizeof(pipe_t));
  ba->from = b;
  ba->to = a;
  thread_create(pipe_thread, ba, 256, "pipe", TASK_DETACHED, 4);
}
