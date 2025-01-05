#include <mios/service.h>
#include <mios/suspend.h>
#include <mios/stream.h>
#include <mios/task.h>
#include <mios/cli.h>

#include <assert.h>
#include <stdlib.h>
#include <malloc.h>

typedef struct telnet_server {
  stream_t s;

  stream_t *net;

  uint8_t iac_state;

  uint8_t stream_closed;

} telnet_server_t;


static const uint8_t telnet_init[] = {
  255, 251, 1,  // WILL ECHO
  255, 251, 0,  // WILL BINARY
  255, 251, 3,  // WILL SUPRESS-GO-AHEAD
};



__attribute__((noreturn))
static void *
telnetd_thread(void *arg)
{
  telnet_server_t *ts = arg;
  stream_t *s = &ts->s;

  stream_write(ts->net, telnet_init, sizeof(telnet_init), 0);

  cli_on_stream(s, '>');
  if (!ts->stream_closed)
    stream_close(s);
  wakelock_release();
  free(ts);
  thread_exit(NULL);
}


static ssize_t
telnet_stream_read(struct stream *s, void *buf, size_t size, size_t requested)
{
  telnet_server_t *ts = (telnet_server_t *)s;

  int r;
  while(1) {
    r = stream_read(ts->net, buf, size, requested);
    if(r > 0) {
      uint8_t *b = buf;
      size_t wrptr = 0;
      size_t cut = 0;
      for(size_t i = 0; i < r; i++) {
        uint8_t c = b[i];
        switch(ts->iac_state) {
        case 0:
          if(c == 255) {
            cut++;
            ts->iac_state = c;
          } else {
            if(c)
              b[wrptr++] = c;
            else
              cut++;
          }
          break;
        case 255:
          switch(c) {
          case 255:
            b[wrptr++] = c;
            break;
          case 240 ... 250:
            c = 0;
            // FALLTHRU
          default:
            ts->iac_state = c;
            cut++;
            break;
          }
          break;
        default:
          cut++;
          ts->iac_state = 0;
        }
      }
      r -= cut;
      if(r == 0 && requested) {
        continue;
      }
    }
    return r;
  }
}

static ssize_t
telnet_stream_write(stream_t *s, const void *buf, size_t size, int flags)
{
  // Transform LF -> CRLF

  telnet_server_t *ts = (telnet_server_t *)s;
  static const uint8_t crlf[2] = {0x0d, 0x0a};

  if(buf == NULL) {
    return stream_write(ts->net, buf, size, flags);
  } else {

    size_t written = 0;

    // NO_WAIT is not really supported here.
    // We need to keep track if any of the writes below does a partial
    // write and if so terminate early. In particular it get tricky
    // if we write a partial CRLF because it doesn't translate 1:1
    // to input bytes. So for that we need to keep some kind of state
    assert((flags & STREAM_WRITE_NO_WAIT) == 0);

    const uint8_t *c = buf;
    size_t s0 = 0, i;
    for(i = 0; i < size; i++) {
      if(c[i] == 0x0a) {
        size_t len = i - s0;
        written += stream_write(ts->net, buf + s0, len, flags);
        stream_write(ts->net, crlf, 2, flags);
        written++;
        s0 = i + 1;
      }
    }
    size_t len = i - s0;
    if(len) {
      written += stream_write(ts->net, buf + s0, len, flags);
    }
    return written;
  }
}


static void
telnet_stream_close(stream_t *s)
{
  telnet_server_t *ts = (telnet_server_t *)s;
  stream_close(ts->net);
  ts->stream_closed = 1;
}


static const stream_vtable_t telnet_protocol_vtable = {
  .read = telnet_stream_read,
  .write = telnet_stream_write,
  .close = telnet_stream_close
};


static error_t
telnet_open(stream_t *s)
{
  telnet_server_t *ts = xalloc(sizeof(telnet_server_t), 0, MEM_MAY_FAIL);
  if(ts == NULL)
    return ERR_NO_MEMORY;
  ts->iac_state = 0;
  ts->net = s;
  ts->s.vtable = &telnet_protocol_vtable;
  ts->stream_closed = 0;
  wakelock_acquire();
  error_t r = thread_create_shell(telnetd_thread, ts, "telnetd", &ts->s);
  if(r) {
    wakelock_release();
    free(ts);
    return r;
  }
  return 0;
}

SERVICE_DEF_STREAM("telnet", 23, telnet_open);
