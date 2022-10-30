#include <mios/eventlog.h>
#if EVENTLOG_SIZE
#include <mios/fmt.h>
#include <mios/cli.h>
#include <mios/task.h>
#include <mios/mios.h>

#include <sys/param.h>

#include <stdint.h>
#include <stddef.h>
#include <unistd.h>
#include <stdarg.h>

#define EVENTLOG_MASK (EVENTLOG_SIZE - 1)

typedef struct follower {
  LIST_ENTRY(follower) link;
  uint16_t ptr;
} follower_t;

typedef struct {
  uint64_t ts_tail;
  uint64_t ts_head;
  mutex_t mutex;
  cond_t cond;
  LIST_HEAD(, follower) followers;
  uint16_t head;
  uint16_t tail;
  uint8_t data[EVENTLOG_SIZE];
} evlogfifo_t;

static evlogfifo_t ef0;

/**
 * Message structure
 *
 *  u8 [length]
 *  u8 [flags]    --TTTLLL
 *                  T Length of timestamp delta
 *                  L Log level
 *
 *  var [message]
 *  var [timestamp delta]
 */

static uint16_t
evl_used(const evlogfifo_t *ef)
{
  return ef->head - ef->tail;
}


static size_t
evl_avail(const evlogfifo_t *ef)
{
  return EVENTLOG_SIZE - evl_used(ef);
}


static int64_t
evl_read_delta_ts(evlogfifo_t *ef, uint16_t ptr)
{
  const int len = ef->data[ptr & EVENTLOG_MASK];
  const int tlen = (ef->data[(ptr + 1) & EVENTLOG_MASK] >> 3) & 7;
  ptr += len - tlen;

  int64_t r = 0;
  for(int i = tlen - 1; i >= 0; i--) {
    r = (r << 8) | ef->data[(ptr + i) & EVENTLOG_MASK];
  }
  return r;
}


static void
evl_erase_tail(evlogfifo_t *ef)
{
  if(ef->head == ef->tail)
    return;
  ef->ts_tail += evl_read_delta_ts(ef, ef->tail);
  int len = ef->data[ef->tail & EVENTLOG_MASK];
  follower_t *f;
  LIST_FOREACH(f, &ef->followers, link) {
    if(f->ptr == ef->tail)
      f->ptr += len;
  }
  ef->tail += len;
}


static void
evl_ensure(evlogfifo_t *ef, size_t size)
{
  while(evl_avail(ef) < size) {
    evl_erase_tail(ef);
  }
}


static size_t
evl_fmt_cb(void *aux, const char *s, size_t len)
{
  evlogfifo_t *ef = aux;

  const int head = ef->head;
  const int curlen = ef->data[head & EVENTLOG_MASK];
  const int to_copy = MAX(MIN(len, 128 - curlen), 0);

  evl_ensure(ef, curlen + to_copy);

  for(int i = 0; i < to_copy; i++) {
    ef->data[(head + curlen + i) & EVENTLOG_MASK] = s[i];
  }
  ef->data[head & EVENTLOG_MASK] = curlen + to_copy;
  return to_copy;
}


void
evlog(event_level_t level, const char *fmt, ...)
{
  evlogfifo_t *ef = &ef0;
  int64_t now = clock_get();

  mutex_lock(&ef->mutex);

  evl_ensure(ef, 2);

  if(ef->head == ef->tail) {
    ef->ts_tail = now;
    ef->ts_head = now;
  }

  const int head = ef->head;
  ef->data[(head + 0) & EVENTLOG_MASK] = 2;
  va_list ap;
  va_start(ap, fmt);
  fmtv(evl_fmt_cb, ef, fmt, ap);
  va_end(ap);

  int msg_len = ef->data[(head + 0) & EVENTLOG_MASK];
  int64_t delta_ts = now - ef->ts_head;
  int64_t dts = delta_ts;
  int ts_len = 0;
  while(dts) {
    ts_len++;
    dts >>= 8;
  }

  evl_ensure(ef, msg_len + ts_len);
  for(int i = 0; i < ts_len; i++) {
    ef->data[(head + msg_len + i) & EVENTLOG_MASK] = delta_ts;
    delta_ts >>= 8;
  }

  ef->ts_head = now;
  ef->data[(head + 0) & EVENTLOG_MASK] = msg_len + ts_len;
  ef->data[(head + 1) & EVENTLOG_MASK] = level | (ts_len << 3);
  ef->head += msg_len + ts_len;
  mutex_unlock(&ef->mutex);
}

const char level2str[8][7] = {
  "EMERG ",
  "ALERT ",
  "CRIT  ",
  "ERROR ",
  "WARN  ",
  "NOTICE",
  "INFO  ",
  "DEBUG "
};


static void
dump_log(evlogfifo_t *ef, stream_t *st, int follow)
{

  if(st->read == NULL)
    follow = 0;

  mutex_lock(&ef->mutex);

  follower_t f;
  f.ptr = ef->tail;

  LIST_INSERT_HEAD(&ef->followers, &f, link);

  int64_t ts = ef->ts_tail;

  while(1) {

    uint16_t ptr = f.ptr;

    if(ptr == ef->head) {
      if(!follow)
        break;
      st->write(st, NULL, 0);
      if(cond_wait_timeout(&ef->cond, &ef->mutex, clock_get() + 100000)) {}
      uint8_t dummy;
      int r = st->read(st, &dummy, 1, STREAM_READ_WAIT_NONE);
      if(r)
        break;
      continue;
    }

    const uint8_t len = ef->data[(ptr + 0) & EVENTLOG_MASK];
    const uint8_t flags = ef->data[(ptr + 1) & EVENTLOG_MASK];
    const uint8_t level = flags & 7;
    const uint8_t tlen = (flags >> 3) & 7;
    const uint16_t msglen = len - 2 - tlen;
    const uint16_t msgstart = (ptr + 2) & EVENTLOG_MASK;
    const uint16_t msgend = (msgstart + msglen) & EVENTLOG_MASK;

    ts += evl_read_delta_ts(ef, ptr);

    const int ms_ago = (clock_get() - ts) / 1000;

    stprintf(st, "%5d.%03d %s : ",
             -(ms_ago / 1000), ms_ago % 1000,  level2str[level]);

    if(msgend >= msgstart) {
      st->write(st, ef->data + msgstart, msglen);
    } else {
      st->write(st, ef->data + msgstart, EVENTLOG_SIZE - msgstart);
      st->write(st, ef->data, msgend);
    }
    st->write(st, "\n", 1);

    f.ptr = ptr + len;
  }
  LIST_REMOVE(&f, link);
  mutex_unlock(&ef->mutex);
}

static error_t
cmd_log(cli_t *cli, int argc, char **argv)
{
  dump_log(&ef0, cli->cl_stream, argc > 1);
  return 0;
}

CLI_CMD_DEF("log", cmd_log);

#endif
