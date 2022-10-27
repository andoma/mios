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


typedef struct {
  uint64_t ts_tail;
  uint64_t ts_head;
  mutex_t mutex;
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
  const int len = ef->data[ptr & (EVENTLOG_SIZE - 1)];
  const int tlen = (ef->data[(ptr + 1) & (EVENTLOG_SIZE - 1)] >> 3) & 7;
  ptr += len - tlen;

  int64_t r = 0;
  for(int i = tlen - 1; i >= 0; i--) {
    r = (r << 8) | ef->data[(ptr + i) & (EVENTLOG_SIZE - 1)];
  }
  return r;
}


static void
evl_erase_tail(evlogfifo_t *ef)
{
  if(ef->head == ef->tail)
    return;
  ef->ts_tail += evl_read_delta_ts(ef, ef->tail);
  int len = ef->data[ef->tail & (EVENTLOG_SIZE - 1)];
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
  const int curlen = ef->data[head & (EVENTLOG_SIZE - 1)];
  const int to_copy = MAX(MIN(len, 128 - curlen), 0);

  evl_ensure(ef, curlen + to_copy);

  for(int i = 0; i < to_copy; i++) {
    ef->data[(head + curlen + i) & (EVENTLOG_SIZE - 1)] = s[i];
  }
  ef->data[head & (EVENTLOG_SIZE - 1)] = curlen + to_copy;
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
  ef->data[(head + 0) & (EVENTLOG_SIZE - 1)] = 2;
  va_list ap;
  va_start(ap, fmt);
  fmtv(evl_fmt_cb, ef, fmt, ap);
  va_end(ap);

  int msg_len = ef->data[(head + 0) & (EVENTLOG_SIZE - 1)];
  int64_t delta_ts = now - ef->ts_head;
  int64_t dts = delta_ts;
  int ts_len = 0;
  while(dts) {
    ts_len++;
    dts >>= 8;
  }

  evl_ensure(ef, msg_len + ts_len);
  for(int i = 0; i < ts_len; i++) {
    ef->data[(head + msg_len + i) & (EVENTLOG_SIZE - 1)] = delta_ts;
    delta_ts >>= 8;
  }

  ef->ts_head = now;
  ef->data[(head + 0) & (EVENTLOG_SIZE - 1)] = msg_len + ts_len;
  ef->data[(head + 1) & (EVENTLOG_SIZE - 1)] = level | (ts_len << 3);
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
show_log(cli_t *cli)
{
  evlogfifo_t  *ef = &ef0;

  const int64_t now = clock_get();

  mutex_lock(&ef->mutex);

  uint16_t ptr = ef->tail;
  const uint16_t head = ef->head;

  int64_t ts = ef->ts_tail;

  while(ptr != head) {
    const uint8_t len = ef->data[(ptr + 0) & (EVENTLOG_SIZE - 1)];

    const uint8_t flags = ef->data[(ptr + 1) & (EVENTLOG_SIZE - 1)];
    const uint8_t level = flags & 7;
    const uint8_t tlen = (flags >> 3) & 7;

    int64_t delta = evl_read_delta_ts(ef, ptr);

    ts += delta;
    const uint16_t msglen = len - 2 - tlen;
    const uint16_t msgstart = (ptr + 2) & (EVENTLOG_SIZE - 1);
    const uint16_t msgend = (msgstart + msglen) & (EVENTLOG_SIZE - 1);

    int ms_ago = (now - ts) / 1000;

    cli_printf(cli, "%5d.%03d %s : ",
               -(ms_ago / 1000), ms_ago % 1000,  level2str[level]);

    if(msgend >= msgstart) {
      cli->cl_stream->write(cli->cl_stream, ef->data + msgstart,
                            msglen);
    } else {
      cli->cl_stream->write(cli->cl_stream, ef->data + msgstart,
                            EVENTLOG_SIZE - msgstart);
      cli->cl_stream->write(cli->cl_stream, ef->data, msgend);
    }
    cli_printf(cli, "\n");

    ptr += len;
  }
  mutex_unlock(&ef->mutex);
}

static error_t
cmd_log(cli_t *cli, int argc, char **argv)
{
  if(argc > 1) {
    evlog(LOG_INFO, "CLI: %s", argv[1]);
    return 0;
  }

  show_log(cli);
  return 0;
}

CLI_CMD_DEF("log", cmd_log);

#endif
