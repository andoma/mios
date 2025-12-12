#include <mios/mios.h>
#include <mios/stream.h>
#include <mios/task.h>
#include <mios/cli.h>
#include <mios/unwind.h>

#include <stdio.h>

#include "irq.h"

void  __attribute__((weak, noreturn))
halt(const char *msg)
{
  printf("%s\n", msg);
  while(1) {}
}

__attribute__((weak))
stream_t *
get_crashlog_stream(void)
{
  return stdio;
}


__attribute__((weak))
void backtrace_print_frame(struct stream *st, void *frame)
{

}

__attribute__((weak))
void
backtrace_print_thread(struct stream *st, struct thread *t)
{

}


static void
panicv(void *frame, const char *fmt, va_list ap)
{
  irq_off();
  fini();

  stream_t *st = get_crashlog_stream();
  stprintf(st, "\n\nPANIC: ");
  vstprintf(st, fmt, ap);
  va_end(ap);
  stprintf(st, "\n");

  backtrace_print_frame(st, frame);

  stream_write(st, NULL, 0, 0); // Stream flush
  cli_console('#');
}

void
panic(const char *fmt, ...)
{
  va_list ap;
  va_start(ap, fmt);
  panicv(NULL, fmt, ap);
  va_end(ap);
  halt(fmt);
}

void
panic_frame(void *frame, const char *fmt, ...)
{
  va_list ap;
  va_start(ap, fmt);
  panicv(frame, fmt, ap);
  va_end(ap);
  halt(fmt);
}



void
__assert_func(const char *expr, const char *file, int line)
{
  panic("ASSERT: %s at %s:%d\n", expr, file, line);
}

