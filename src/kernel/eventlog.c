#include <mios/eventlog.h>
#if EVENTLOG_SIZE
#include <mios/fmt.h>
#include <mios/cli.h>
#include <mios/task.h>
#include <mios/mios.h>
#include <mios/service.h>

#include <sys/param.h>

#include "net/pbuf.h"

#include <stdint.h>
#include <stddef.h>
#include <unistd.h>
#include <stdarg.h>
#include <stdlib.h>
#include <malloc.h>
#include <string.h>

#define EVENTLOG_MASK (EVENTLOG_SIZE - 1)

typedef struct follower {
  LIST_ENTRY(follower) link;
  void (*cb)(struct follower *f);
  uint16_t ptr;
  uint16_t drops;
} follower_t;

typedef struct {
  uint64_t ts_tail;
  uint64_t ts_head;
  mutex_t mutex;
  LIST_HEAD(, follower) followers;
  uint16_t head;
  uint16_t tail;
  uint32_t seq_tail;
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
  ef->seq_tail++;
  int len = ef->data[ef->tail & EVENTLOG_MASK];
  follower_t *f;
  LIST_FOREACH(f, &ef->followers, link) {
    if(f->ptr == ef->tail) {
      f->ptr += len;
      f->drops++;
    }
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
  const int to_copy = MAX(MIN(len, 100 - curlen), 0);

  evl_ensure(ef, curlen + to_copy);

  for(int i = 0; i < to_copy; i++) {
    ef->data[(head + curlen + i) & EVENTLOG_MASK] = s[i];
  }
  ef->data[head & EVENTLOG_MASK] = curlen + to_copy;
  return to_copy;
}


void
evlog0(event_level_t level, stream_t *st, const char *fmt, ...)
{
  evlogfifo_t *ef = &ef0;
  int64_t now = clock_get();
  va_list ap;
  va_start(ap, fmt);


  if(st != NULL) {
    va_list ap2;
    va_copy(ap2, ap);
    vstprintf(st, fmt, ap2);
    va_end(ap2);
    st->write(st, "\n", 1);
  }

  mutex_lock(&ef->mutex);

  evl_ensure(ef, 2);

  if(ef->head == ef->tail) {
    ef->ts_tail = now;
    ef->ts_head = now;
  }

  const int head = ef->head;
  ef->data[(head + 0) & EVENTLOG_MASK] = 2;
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

  follower_t *f;
  LIST_FOREACH(f, &ef->followers, link) {
    f->cb(f);
  }

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


typedef struct stream_follower {
  follower_t f;
  cond_t c;
} stream_follower_t;

static void
stream_follower_wakeup(follower_t *f)
{
  stream_follower_t *st = (stream_follower_t *)f;
  cond_signal(&st->c);
}

static void
stream_log(evlogfifo_t *ef, stream_t *st, int follow)
{
  if(st->read == NULL)
    follow = 0;

  mutex_lock(&ef->mutex);

  stream_follower_t sf;
  sf.f.ptr = ef->tail;
  sf.f.cb = stream_follower_wakeup;
  cond_init(&sf.c, "log");

  LIST_INSERT_HEAD(&ef->followers, &sf.f, link);

  int64_t ts = ef->ts_tail;

  while(1) {

    uint16_t ptr = sf.f.ptr;

    if(ptr == ef->head) {
      if(!follow)
        break;
      st->write(st, NULL, 0);
      if(cond_wait_timeout(&sf.c, &ef->mutex, clock_get() + 100000)) {}
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

    sf.f.ptr = ptr + len;
  }
  LIST_REMOVE(&sf.f, link);
  mutex_unlock(&ef->mutex);
}

static error_t
cmd_log(cli_t *cli, int argc, char **argv)
{
  stream_log(&ef0, cli->cl_stream, argc > 1);
  return 0;
}

CLI_CMD_DEF("log", cmd_log);

static error_t
cmd_mark(cli_t *cli, int argc, char **argv)
{
  int cnt = 1;
  if(argc > 2)
    cnt = atoi(argv[2]);
  for(int i = 0; i < cnt; i++) {
    evlog(LOG_INFO, "%s", argv[1]);
  }
  return 0;
}

CLI_CMD_DEF("mark", cmd_mark);




typedef struct {
  follower_t f;
  uint64_t ts;
  uint32_t seq;

  void *opaque;
  service_event_cb_t *cb;
  size_t max_fragment_size;

} evlog_svc_follower_t;



/**
 * Framing;
 *
 *   rDTT_TLLL
 *                r = reserved
 *                D = Discontinuity
 *                T = Size of timestamp delta
 *                L = Log level (0 - 7)
 *
 *    [4 byte sequence, if D is set]
 *    [Timestamp Delta, up to 8 bytes]
 *    [Message]
 *
 */


static pbuf_t *
evlog_svc_pull(void *opaque)
{
  evlog_svc_follower_t *esf = opaque;
  evlogfifo_t *ef = &ef0;

  pbuf_t *pb = NULL;
  mutex_lock(&ef->mutex);

  uint16_t ptr = esf->f.ptr;

  if(ptr != ef->head) {

    uint8_t hdr = 0;

    uint64_t ts = esf->ts;
    if(ptr == ef->tail) {
      ts = ef->ts_tail;
      hdr = 0x40;
    }

    const uint8_t len = ef->data[(ptr + 0) & EVENTLOG_MASK];
    const uint8_t flags = ef->data[(ptr + 1) & EVENTLOG_MASK];
    const uint8_t level = flags & 7;
    const uint8_t tlen = (flags >> 3) & 7;
    const uint16_t msglen = len - 2 - tlen;

    const uint16_t msgstart = (ptr + 2) & EVENTLOG_MASK;
    const uint16_t msgend = (msgstart + msglen) & EVENTLOG_MASK;

    ts += evl_read_delta_ts(ef, ptr);
    uint64_t ms_ago = (clock_get() - ts) / 1000;

    uint8_t tsbuf[8];
    int tslen = 0;
    for(uint32_t i = 0; i < 8; i++) {
      if(ms_ago == 0)
        break;
      tsbuf[i] = ms_ago;
      ms_ago >>= 8;
      tslen++;
    }

    hdr |= level;
    hdr |= tslen << 3;

    pb = pbuf_make(0, 0);
    if(pb != NULL) {
      pb = pbuf_write(pb, &hdr, 1, esf->max_fragment_size);

      if(hdr & 0x40) {
        pb = pbuf_write(pb, &ef->seq_tail, 4, esf->max_fragment_size);
      }
      pb = pbuf_write(pb, tsbuf, tslen, esf->max_fragment_size);

      if(msgend >= msgstart) {
        pb = pbuf_write(pb, ef->data + msgstart, msglen,
                        esf->max_fragment_size);
      } else {
        pb = pbuf_write(pb, ef->data + msgstart, EVENTLOG_SIZE - msgstart,
                        esf->max_fragment_size);
        pb = pbuf_write(pb, ef->data, msgend, esf->max_fragment_size);
      }
    }
    if(pb != NULL) {
      esf->ts = ts;
      esf->f.ptr += len;
    }
  }

  mutex_unlock(&ef->mutex);
  return pb;
}


static void
evlog_svc_wakeup(follower_t *f)
{
  evlog_svc_follower_t *svc = (evlog_svc_follower_t *)f;
  svc->cb(svc->opaque, SERVICE_EVENT_WAKEUP);
}

static void *
evlog_svc_open(void *opaque, service_event_cb_t *cb, size_t max_fragment_size,
               service_get_flow_header_t *get_flow_hdr)
{
  evlog_svc_follower_t *esf =
    xalloc(sizeof(evlog_svc_follower_t), 0, MEM_MAY_FAIL);
  if(esf == NULL)
    return NULL;

  memset(esf, 0, sizeof(evlog_svc_follower_t));

  esf->opaque = opaque;
  esf->cb = cb;
  esf->max_fragment_size = max_fragment_size;

  evlogfifo_t *ef = &ef0;

  mutex_lock(&ef->mutex);
  esf->f.ptr = ef->tail;
  esf->f.cb = evlog_svc_wakeup;
  LIST_INSERT_HEAD(&ef->followers, &esf->f, link);
  mutex_unlock(&ef->mutex);

  return esf;
}


static void
evlog_svc_close(void *opaque)
{
  evlog_svc_follower_t *esf = opaque;
  evlogfifo_t *ef = &ef0;

  mutex_lock(&ef->mutex);
  LIST_REMOVE(&esf->f, link);
  mutex_unlock(&ef->mutex);

  esf->cb(esf->opaque, SERVICE_EVENT_CLOSE);
  free(esf);
}

SERVICE_DEF("log",
            evlog_svc_open, NULL, NULL, evlog_svc_pull, evlog_svc_close);

#endif
