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
shell_open(stream_t *s)
{
  wakelock_acquire();
  error_t r = thread_create_shell(shell_thread, s, "shell", s);
  if(r) {
    wakelock_release();
    return r;
  }
  return 0;
}

static const service_t svc_shell
__attribute__ ((used, section("servicedef"))) = {
  .name = "shell",
  .ble_psm = 23, // BLE L2CAP CoC: PSM 0x97 (packet) / 0xd7 (stream reframing)
  .open_stream = shell_open,
};
