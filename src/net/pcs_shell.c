#include <stdarg.h>

#include <mios/cli.h>
#include <mios/error.h>
#include <stdio.h>
#include <malloc.h>

#include "pcs/pcs.h"
#include "pcs_shell.h"

typedef struct {
  stream_t s;
  pcs_t *pcs;

} pcs_shell_stream_t;


static int
pcs_shell_read(struct stream *s, void *buf, size_t size, int wait)
{
  pcs_shell_stream_t *pss = (pcs_shell_stream_t *)s;

  return pcs_read(pss->pcs, buf, size,
                  wait == STREAM_READ_WAIT_ALL ? size : wait);
}


static void
pcs_shell_write(struct stream *s, const void *buf, size_t size)
{
  pcs_shell_stream_t *pss = (pcs_shell_stream_t *)s;
  if(size)
    pcs_send(pss->pcs, buf, size);
  else
    pcs_flush(pss->pcs);
}


static void *
pcs_shell(void *arg)
{
  pcs_shell_stream_t pss;
  pss.s.read = pcs_shell_read;
  pss.s.write = pcs_shell_write;
  pss.pcs = arg;

  cli_on_stream(&pss.s, '>');
  pcs_close(pss.pcs);
  return NULL;
}


int
pcs_shell_create(pcs_t *pcs)
{
  int flags = TASK_DETACHED;
#ifdef HAVE_FPU
  flags |= TASK_FPU;
#endif
  return !task_create(pcs_shell, pcs, 1024, "remotecli", flags, 0);
}


void *
pcs_malloc(size_t size)
{
  return xalloc(size, 0, MEM_MAY_FAIL);
}
