#include <mios/service.h>
#include <mios/stream.h>
#include <mios/task.h>
#include <stdio.h>
#include <unistd.h>

__attribute__((noreturn))
static void *
echo_thread(void *arg)
{
  stream_t *s = arg;
  char buf[64];
  while(1) {
    ssize_t r;
    r = stream_read(s, buf, sizeof(buf), 0);
    if(r == 0) {
      stream_write(s, NULL, 0, 0);
      r = stream_read(s, buf, sizeof(buf), 1);
    }
    if(r < 0)
      break;
    if(r > 0) {
      stream_write(s, buf, r, 0);
    }
  }
  stream_close(s);
  thread_exit(NULL);
}

static error_t
echo_open_stream(stream_t *s)
{
  thread_t *t = thread_create(echo_thread, s, 1024, "echo", TASK_DETACHED, 5);
  if(t)
    return 0;
  return ERR_NO_MEMORY;
}

SERVICE_DEF_STREAM("echo", 7, echo_open_stream);
