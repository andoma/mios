#if 0

#include <mios/service.h>
#include <mios/suspend.h>
#include <mios/stream.h>
#include <mios/task.h>
#include <mios/cli.h>

__attribute__((noreturn))
static void *
shell_thread(void *arg)
{
  stream_t *s = arg;

  cli_on_stream(s, '>');
  stream_close(s);
  wakelock_release();
  thread_exit(NULL);
}



static error_t
shell_open_raw(stream_t *s, int is_telnet)
{
  wakelock_acquire();
  error_t r = thread_create_shell(shell_thread, s, "remotecli", s);
  if(r) {
    wakelock_release();
    return r;
  }
  return 0;
}


static error_t
shell_open(stream_t *s)
{
  return shell_open_raw(s, 0);
}

SERVICE_DEF_STREAM("shell", 23, 23, shell_open);
#endif
