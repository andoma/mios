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
  stream_write(stdio, buf, size, flags);

  crashlog_buf_t *cb = cs->buf;

  if(buf == NULL) {
    cb->magic = CRASHLOG_PRESENT;
    return size;
  }

  if(cb->magic != CRASHLOG_READY)
    return size;

  size_t len = strlen(cb->message);
  size_t to_copy = sizeof(cb->message) - len - 1;
  to_copy = MIN(size, to_copy);

  char *dst = cb->message + len;
  const char *src = buf;
  memcpy(dst, src, to_copy);
  dst[to_copy] = 0;
  return size;
}

static const stream_vtable_t crashlog_stream_vtable = {
  .write = crashlog_stream_write,
};

static const crashlog_stream_t crashlog_stream = {
  .s = {
    .vtable = &crashlog_stream_vtable
  },
  .buf = (void *)CRASHLOG_ADDR
};

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

  if(cb->magic == CRASHLOG_PRESENT) {
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
