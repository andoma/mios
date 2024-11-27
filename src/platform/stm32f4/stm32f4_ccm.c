#include <stdio.h>
#include <malloc.h>
#include <stdio.h>
#include <string.h>

#include <sys/param.h>

#include <mios/eventlog.h>

#include "stm32f4_clk.h"
#include "cpu.h"

#define PANIC_PREP  0xc0dedbad
#define PANIC_MAGIC 0xabadc0de

struct panic_buf {
  uint32_t magic;
  char message[256 - 4];
};

static struct panic_buf *const panic_buf = (void *)0x1000ff00;

static void  __attribute__((constructor(200)))
stm32f4_ccm_init(void)
{
  // CCM
  // Note: First bytes of CCM are reserved for cpu_t and hardwired to
  // this address via the curcpu() macro in stm32f4_ccm.h
  // Last 256 bytes are used as panic-buffer as CCM is not cleared on reset

  clk_enable(CLK_CCMDATARAMEN);
  heap_add_mem(0x10000000 + sizeof(cpu_t), 0x1000ff00, MEM_TYPE_LOCAL);

  if(panic_buf->magic == PANIC_MAGIC) {
    evlog(LOG_ERR, "Previous panic: %s", panic_buf->message);
  }
  panic_buf->magic = PANIC_PREP;
  panic_buf->message[0] = 0;
}

static ssize_t
panic_stream_write(struct stream *s, const void *buf, size_t size, int flags)
{
  stream_write(stdio, buf, size, flags);

  if(buf == NULL) {
    panic_buf->magic = PANIC_MAGIC;
    return size;
  }

  if(panic_buf->magic != PANIC_PREP)
    return size;

  size_t len = strlen(panic_buf->message);
  size_t to_copy = sizeof(panic_buf->message) - len - 1;
  to_copy = MIN(size, to_copy);

  char *dst = panic_buf->message + len;
  const char *src = buf;
  // Copy and strip out newlines and other control characters
  for(int i = 0; i < to_copy; i++, src++) {
    if(*src < 32)
      continue;
    *dst++ = *src;
  }
  *dst = 0;
  return size;
}

static const stream_vtable_t panic_stream_vtable = {
  .write = panic_stream_write,
};

static const stream_t panic_stream = {
  .vtable = &panic_stream_vtable
};

stream_t *
get_panic_stream(void)
{
  return (stream_t *)&panic_stream;
}
