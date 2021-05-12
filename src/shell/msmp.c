#include <sys/param.h>
#include <string.h>

#include <util/hdlc.h>

#include <mios/cli.h>
#include <mios/task.h>
#include <mios/fifo.h>

#include "msmp.h"

#define WRITE_BUF_SIZE 16

typedef struct msmp {

  stream_t console;

  stream_t *mux;

  mutex_t send_mutex;
  mutex_t read_mutex;

  task_t *task_read;
  task_t *task_console;

  cond_t read_fifo_cond;

  FIFO_DECL(read_fifo, 16);

  uint8_t write_buf[WRITE_BUF_SIZE];
  uint8_t write_fill;

} msmp_t;


static void
msmp_input(void *opaque, const uint8_t *data, size_t len)
{
  msmp_t *m = opaque;

  if(len < 1)
    return;
  if(data[0] == 0) {
    // Console

    data += 1;
    len -= 1;

    mutex_lock(&m->read_mutex);

    for(size_t i = 0; i < len; i++) {
      if(fifo_is_full(&m->read_fifo))
        break;
      fifo_wr(&m->read_fifo, *data++);
    }
    cond_signal(&m->read_fifo_cond);
    mutex_unlock(&m->read_mutex);
  }
}



static void * __attribute__((noreturn))
msmp_read_thread(void *arg)
{
  msmp_t *m = arg;
  hdlc_read(m->mux, msmp_input, arg, 16);
}


static int
msmp_console_read(struct stream *s, void *buf, size_t size, int wait)
{
  msmp_t *m = (msmp_t *)s;
  char *d = buf;
  mutex_lock(&m->read_mutex);

  for(size_t i = 0; i < size; i++) {
    while(fifo_is_empty(&m->read_fifo)) {
      if(stream_wait_is_done(wait, i, size)) {
        mutex_unlock(&m->read_mutex);
        return i;
      }
      cond_wait(&m->read_fifo_cond, &m->read_mutex);
    }
    d[i] = fifo_rd(&m->read_fifo);
  }
  mutex_unlock(&m->read_mutex);
  return size;
}


static void
msmp_console_flush(msmp_t *m)
{
  if(m->write_fill == 0)
    return;

  uint8_t hdr[1] = {0};

  struct iovec vec[2] = {
    {
      .iov_base = hdr,
      .iov_len = sizeof(hdr)
    }, {
      .iov_base = m->write_buf,
      .iov_len = m->write_fill
    }
  };
  hdlc_sendv(m->mux, vec, 2);
  m->write_fill = 0;
}


static int
have_lf(const uint8_t *buf, size_t len)
{
  for(size_t i = 0; i < len; i++) {
    if(buf[i] == '\n') {
      return 1;
    }
  }
  return 0;
}


static void
msmp_console_write(struct stream *s, const void *buf, size_t size)
{
  msmp_t *m = (msmp_t *)s;

  mutex_lock(&m->send_mutex);

  if(size == 0) {
    msmp_console_flush(m);
  } else {

    while(size) {
      const int cs = MIN(size, WRITE_BUF_SIZE - m->write_fill);
      memcpy(m->write_buf + m->write_fill, buf, cs);

      int flush = have_lf(buf, cs);
      m->write_fill += cs;
      buf += cs;
      size -= cs;

      if(m->write_fill == WRITE_BUF_SIZE || flush)
        msmp_console_flush(m);
    }
  }

  mutex_unlock(&m->send_mutex);
}

static msmp_t g_msmp = {
  .console.write = msmp_console_write,
  .console.read = msmp_console_read,
  .send_mutex = MUTEX_INITIALIZER("msmp"),
  .read_mutex = MUTEX_INITIALIZER("msmp"),
  .read_fifo_cond = COND_INITIALIZER("msmp"),
};



static void *
msmp_shell_thread(void *arg)
{
  msmp_t *m = arg;
  while(1) {
    if(cli_on_stream(&m->console) < 0)
      break;
  }
  return NULL;
}



void
msmp_init(stream_t *mux, int flags)
{
  msmp_t *m = &g_msmp;

  m->mux = mux;

  if(flags & MSMP_CONSOLE) {
    int flags = TASK_DETACHED;
#ifdef HAVE_FPU
    flags |= TASK_FPU;
#endif
    m->task_console = task_create(msmp_shell_thread, m, 1024, "msmpcli",
                                  flags, 1);
  }

  if(m->task_console != NULL)
    m->task_read = task_create(msmp_read_thread, m, 256, "msmp", 0, 8);
}


void
msmp_send(uint8_t hdrbyte, const void *data, size_t len)
{
  msmp_t *m = &g_msmp;
  if(m->mux == NULL)
    return;

  mutex_lock(&m->send_mutex);

  uint8_t hdr[1] = {hdrbyte};

  struct iovec vec[2] = {
    {
      .iov_base = hdr,
      .iov_len = sizeof(hdr)
    }, {
      .iov_base = (void *)data,
      .iov_len = len,
    }
  };
  hdlc_sendv(m->mux, vec, 2);

  mutex_unlock(&m->send_mutex);
}
