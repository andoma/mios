/*
 * Minimal write-only stdio for the hostlib shared object: printf/log go to
 * fd 1 via a raw write. No stdin, no SIGIO -- a .so must not install
 * process-global signal handlers behind its host's back.
 */
#include <stdio.h>
#include <mios/stream.h>
#include "linux.h"

static ssize_t
lib_write(struct stream *s, const void *buf, size_t size, int flags)
{
  (void)s; (void)flags;
  const uint8_t *d = buf;
  size_t off = 0;
  while(off < size) {
    long n = linux_syscall(SYS_write, 1, d + off, size - off);
    if(n == -LINUX_EINTR)
      continue;
    if(n < 0)
      break;
    off += n;
  }
  return size;
}

static const stream_vtable_t lib_console_vtable = { .write = lib_write };
static stream_t lib_console = { &lib_console_vtable };

static void __attribute__((constructor(110)))
console_lib_init(void)
{
  stdio = &lib_console;
}
