#include <stdarg.h>

#include <mios/cli.h>
#include <mios/error.h>
#include <stdio.h>

#include "pcs/pcs.h"
#include "pcs_shell.h"

static size_t
remote_output(void *arg, const char *s, size_t len)
{
  if(len)
    pcs_send(arg, s, len, 0);
  return len;
}

static void
remote_printf(struct cli *cli, const char *fmt, ...)
{
  va_list ap;
  va_start(ap, fmt);
  fmtv(remote_output, cli->cl_opaque, fmt, ap);
  va_end(ap);
}

static int
remote_getc(struct cli *cli, int wait)
{
  char c;
  int r = pcs_read(cli->cl_opaque, &c, 1, wait);
  if(r == 0)
    return ERR_NOT_READY;
  if(r != 1)
    return ERR_RX;
  return c;
}


static void *
pcs_shell(void *arg)
{
  pcs_t *pcs = arg;

  cli_t cli = {};
  cli.cl_printf = remote_printf;
  cli.cl_getc = remote_getc;
  cli.cl_opaque = pcs;


  cli_prompt(&cli);

  while(1) {
    char c;
    int r = pcs_read(pcs, &c, 1, 1);
    if(r < 1 || c == 4) // ^D
      break;

    cli_input_char(&cli, c);
  }
  pcs_close(pcs);
  return NULL;
}


int
pcs_shell_create(pcs_t *pcs)
{
  task_create(pcs_shell, pcs, 1024, "remotecli",
              TASK_FPU | TASK_DETACHED, 0);
  return 0;
}
