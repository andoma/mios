#include <mios/eventlog.h>
#include <mios/fmt.h>
#include <mios/cli.h>
#include <sys/param.h>
#include <stdint.h>
#include <stddef.h>
#include <unistd.h>
#include <stdarg.h>

#ifndef EVENTLOG_SIZE
#define EVENTLOG_SIZE 512
#endif

typedef struct {
  uint64_t ts_head;
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
 *                  T Length of timestamp
 *                  L Log level
 *
 *  var [message]
 *  var [timestamp]
 *  u8  [length]
 */

static size_t
evl_used(const evlogfifo_t *ef)
{
  return (ef->head - ef->tail) & (EVENTLOG_SIZE - 1);
}

static size_t
evl_avail(const evlogfifo_t *ef)
{
  return EVENTLOG_SIZE - evl_used(ef);
}

static void
evl_erase_tail(evlogfifo_t *ef)
{
  if(ef->head == ef->tail)
    return;
  ef->tail += ef->data[ef->tail & (EVENTLOG_SIZE - 1)];
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

  ef->ts_head = now;
  evl_ensure(ef, 2);

  const int head = ef->head;

  ef->data[(head + 0) & (EVENTLOG_SIZE - 1)] = 2;
  ef->data[(head + 1) & (EVENTLOG_SIZE - 1)] = level;

  va_list ap;
  va_start(ap, fmt);
  fmtv(evl_fmt_cb, ef, fmt, ap);
  va_end(ap);

  evl_ensure(ef, 1);

  int total_len = ef->data[head & (EVENTLOG_SIZE - 1)] + 1;
  ef->data[head & (EVENTLOG_SIZE - 1)] = total_len;
  ef->data[(head + total_len - 1) & (EVENTLOG_SIZE - 1)] = total_len;
}


static error_t
cmd_log(cli_t *cli, int argc, char **argv)
{

}

CLI_CMD_DEF("log", cmd_log);
