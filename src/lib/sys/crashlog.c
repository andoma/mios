#include <string.h>
#include <mios/stream.h>
#include <mios/eventlog.h>
#include <sys/param.h>

#define CRASHLOG_READY   0xc0dedbad
#define CRASHLOG_PRESENT 0xabadc0de

typedef struct {
  uint32_t magic;
  char message[CRASHLOG_SIZE - 4];
} crashlog_buf_t;

typedef struct {
  stream_t s;
  crashlog_buf_t *buf;
} crashlog_stream_t;


static ssize_t
crashlog_stream_write(struct stream *s, const void *buf, size_t size, int flags)
{
  crashlog_stream_t *cs = (crashlog_stream_t *)s;
#ifndef CRASHLOG_DEFER_CONSOLE
  stream_write(stdio, buf, size, flags);
#endif

  crashlog_buf_t *cb = cs->buf;
  if(cb == NULL) {
#ifdef CRASHLOG_DEFER_CONSOLE
    stream_write(stdio, buf, size, flags);
#endif
    return size;
  }

  if(buf == NULL) {
    cb->magic = CRASHLOG_PRESENT;
#ifdef CRASHLOG_DEFER_CONSOLE
    CRASHLOG_COMMIT_BARRIER();
    // Retention is committed BEFORE touching the console. Only the
    // retained prefix is replayed if the backtrace exceeded the buffer.
    cb->message[sizeof(cb->message) - 1] = 0;
    stream_write(stdio, cb->message, strlen(cb->message), flags);
    stream_write(stdio, NULL, 0, flags);
#endif
    return size;
  }

  if(cb->magic != CRASHLOG_READY
#ifdef CRASHLOG_DEFER_CONSOLE
     && cb->magic != CRASHLOG_PRESENT
#endif
     )
    return size;

  size_t len = 0;
  while(len < sizeof(cb->message) - 1 && cb->message[len])
    len++;
  size_t to_copy = sizeof(cb->message) - len - 1;
  to_copy = MIN(size, to_copy);

  char *dst = cb->message + len;
  const char *src = buf;
  memcpy(dst, src, to_copy);
  dst[to_copy] = 0;
#ifdef CRASHLOG_DEFER_CONSOLE
  // Each chunk is independently recoverable, including a panic whose
  // stack unwind faults or stalls before reaching the final flush.
  CRASHLOG_COMMIT_BARRIER();
  cb->magic = CRASHLOG_PRESENT;
  CRASHLOG_COMMIT_BARRIER();
#endif
  return size;
}

static const stream_vtable_t crashlog_stream_vtable = {
  .write = crashlog_stream_write,
};

// The buffer address is set by the platform at init, since on some parts
// it depends on the RAM size read from the chip at runtime.
static crashlog_stream_t crashlog_stream = {
  .s = {
    .vtable = &crashlog_stream_vtable
  },
};

static void
crashlog_init(void *addr)
{
  crashlog_stream.buf = addr;
}

stream_t *
get_crashlog_stream(void)
{
  get_crashlog_stream_prep();
  return (stream_t *)&crashlog_stream.s;
}

static void
crashlog_recover(void)
{
  crashlog_buf_t *cb = crashlog_stream.buf;
  if(cb == NULL)
    return;

  if(cb->magic == CRASHLOG_PRESENT) {
    cb->message[sizeof(cb->message) - 1] = 0;
    char *s = cb->message;

    evlog(LOG_ALERT, "Crashlog from last boot");

    while(1) {
      char *n = strchr(s, '\n');
      if(n != NULL) {
        *n = 0;
      }

      if(*s) {
        evlog(LOG_ALERT, "%s", s);
      }

      if(n == NULL)
        break;
      s = n + 1;
    }
  }
  cb->magic = CRASHLOG_READY;
  memset(cb->message, 0, sizeof(cb->message));
}
