#include <mios/eventlog.h>
#if EVENTLOG_SIZE
#include <mios/fmt.h>
#include <mios/cli.h>
#include <mios/task.h>
#include <mios/mios.h>
#include <mios/service.h>
#include <mios/datetime.h>

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
  int64_t ts_tail;
  int64_t ts_head;
  mutex_t mutex;
  LIST_HEAD(, follower) followers;
  uint16_t head;
  uint16_t tail;
  uint32_t seq_tail;
  uint8_t data[EVENTLOG_SIZE];
} evlogfifo_t;

static evlogfifo_t ef0;


static void __attribute__((constructor(400)))
evlog_init_globals(void)
{
  mutex_init(&ef0.mutex, "evlog");
}

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
  const int max_entry_size = MIN(EVENTLOG_SIZE / 4, 192);
  const int to_copy = MAX(MIN(len, max_entry_size - curlen), 0);

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
  va_list ap;
  va_start(ap, fmt);

  thread_t *t = thread_current();
  stream_t *st = NULL;
  if (t && t->t_stream)
    st = t->t_stream;

  if(st != NULL) {
    va_list ap2;
    va_copy(ap2, ap);
    vstprintf(st, fmt, ap2);
    va_end(ap2);
    stream_write(st, "\n", 1, 0);
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
  if(delta_ts < 0)
    delta_ts = 0;
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
print_timestamp_to_stream(stream_t *st, int64_t ts)
{
  if(wallclock.source) {
    datetime_t dt;
    ts += wallclock.utc_offset;
    int secs = ts / 1000000;
    int usecs = ts % 1000000;
    datetime_from_unixtime(secs + wallclock.tz_offset,
                           &dt);

    stprintf(st, "%04d-%02d-%02d %02d:%02d:%02d.%03d",
             dt.year,
             dt.mon,
             dt.mday,
             dt.hour,
             dt.min,
             dt.sec,
             usecs / 1000);
    if(wallclock.tz_offset == 0) {
      stprintf(st, "Z");
    } else {
      int tzm = wallclock.tz_offset / 60;
      stprintf(st, " +%02d:%02d", tzm / 60, tzm % 60);
    }
  } else {
    int64_t now = clock_get();
    const int ms_ago = (now - ts) / 1000;
    stprintf(st, "%5d.%03d",
             -(ms_ago / 1000), ms_ago % 1000);
  }
}



static void
stream_log(evlogfifo_t *ef, stream_t *st, int follow)
{
  if(st->vtable->read == NULL)
    follow = 0;

  mutex_lock(&ef->mutex);

  stream_follower_t sf;
  sf.f.ptr = ef->tail;
  sf.f.cb = stream_follower_wakeup;
  cond_init(&sf.c, "evlog");

  LIST_INSERT_HEAD(&ef->followers, &sf.f, link);

  int64_t ts = ef->ts_tail;

  while(1) {

    uint16_t ptr = sf.f.ptr;

    if(ptr == ef->head) {
      if(!follow)
        break;
      stream_write(st, NULL, 0, 0);
      if(cond_wait_timeout(&sf.c, &ef->mutex, clock_get() + 100000)) {}
      uint8_t dummy;
      int r = stream_read(st, &dummy, 1, 0);
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

    print_timestamp_to_stream(st, ts);

    stprintf(st, " %s : ", level2str[level]);

    if(msgend >= msgstart) {
      stream_write(st, ef->data + msgstart, msglen, 0);
    } else {
      stream_write(st, ef->data + msgstart, EVENTLOG_SIZE - msgstart, 0);
      stream_write(st, ef->data, msgend, 0);
    }
    stream_write(st, "\n", 1, 0);

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
  if(argc < 2)
    return ERR_INVALID_ARGS;
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

  pushpull_t *p;

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

    pb = pbuf_make(esf->p->preferred_offset, 0);
    const int mfs = esf->p->max_fragment_size;
    if(pb != NULL) {
      pb = pbuf_write(pb, &hdr, 1, mfs);

      if(hdr & 0x40) {
        pb = pbuf_write(pb, &ef->seq_tail, 4, mfs);
      }
      pb = pbuf_write(pb, tsbuf, tslen, mfs);

      if(msgend >= msgstart) {
        pb = pbuf_write(pb, ef->data + msgstart, msglen, mfs);
      } else {
        pb = pbuf_write(pb, ef->data + msgstart, EVENTLOG_SIZE - msgstart, mfs);
        pb = pbuf_write(pb, ef->data, msgend, mfs);
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
  svc->p->net->event(svc->p->net_opaque, PUSHPULL_EVENT_PULL);
}



static void
evlog_svc_close(void *opaque, const char *errmsg)
{
  evlog_svc_follower_t *esf = opaque;
  evlogfifo_t *ef = &ef0;

  mutex_lock(&ef->mutex);
  LIST_REMOVE(&esf->f, link);
  mutex_unlock(&ef->mutex);

  esf->p->net->event(esf->p->net_opaque, PUSHPULL_EVENT_CLOSE);
  free(esf);
}

static const pushpull_app_fn_t evlog_app_fn = {
  .pull = evlog_svc_pull,
  .close = evlog_svc_close
};


static error_t
evlog_svc_open(pushpull_t *p)
{
  evlog_svc_follower_t *esf =
    xalloc(sizeof(evlog_svc_follower_t), 0, MEM_MAY_FAIL);
  if(esf == NULL)
    return ERR_NO_MEMORY;

  memset(esf, 0, sizeof(evlog_svc_follower_t));
  esf->p = p;
  esf->p->app = &evlog_app_fn;
  esf->p->app_opaque = esf;

  evlogfifo_t *ef = &ef0;

  mutex_lock(&ef->mutex);
  esf->f.ptr = ef->tail;
  esf->f.cb = evlog_svc_wakeup;
  LIST_INSERT_HEAD(&ef->followers, &esf->f, link);
  mutex_unlock(&ef->mutex);

  return 0;
}



SERVICE_DEF_PUSHPULL("log", 2, 2, evlog_svc_open);

#include <mios/fs.h>


static size_t g_logfile_max_size;

static size_t
print_timestamp_to_buf(char *buf, size_t buflen, int64_t ts)
{
  size_t used = 0;
  if(wallclock.source) {
    datetime_t dt;
    ts += wallclock.utc_offset;
    int secs = ts / 1000000;
    int usecs = ts % 1000000;
    datetime_from_unixtime(secs + wallclock.tz_offset,
                           &dt);

    used += snprintf(buf, buflen, "%04d-%02d-%02d %02d:%02d:%02d.%03d",
             dt.year,
             dt.mon,
             dt.mday,
             dt.hour,
             dt.min,
             dt.sec,
             usecs / 1000);

    if(used >= buflen)
      return used;

    if(wallclock.tz_offset == 0) {
      used += snprintf(buf + used, buflen - used, "Z");
    } else {
      int tzm = wallclock.tz_offset / 60;
      used += snprintf(buf + used, buflen - used, " +%02d:%02d",
                       tzm / 60, tzm % 60);
    }
    return used;
  } else {
    const int ms = ts / 1000;
    return snprintf(buf, buflen, "%d", ms);
  }
}


__attribute__((noreturn))
static void *
eventlog_to_fs_thread(void *arg)
{
  fs_mkdir("log");
  fs_file_t *f = NULL;
  evlogfifo_t *ef = &ef0;
  mutex_lock(&ef->mutex);

  static stream_follower_t sf;
  static char linebuf[128];

  sf.f.ptr = ef->tail;
  sf.f.cb = stream_follower_wakeup;
  cond_init(&sf.c, "evlog");

  LIST_INSERT_HEAD(&ef->followers, &sf.f, link);

  int64_t sync_deadline = 0;
  int64_t ts = ef->ts_tail;

  while(1) {

    uint16_t ptr = sf.f.ptr;

    if(ptr == ef->head) {

      if(sync_deadline && f) {
        if(cond_wait_timeout(&sf.c, &ef->mutex, sync_deadline)) {
          mutex_unlock(&ef->mutex);
          fs_fsync(f);
          sync_deadline = 0;
          mutex_lock(&ef->mutex);
        }
      } else {
        cond_wait(&sf.c, &ef->mutex);
      }
      continue;
    }

    const uint8_t len = ef->data[(ptr + 0) & EVENTLOG_MASK];
    const uint8_t flags = ef->data[(ptr + 1) & EVENTLOG_MASK];
    const uint8_t level = flags & 7;
    if(level == LOG_DEBUG) {
      // Don't write DEBUG to flash
      sf.f.ptr = ptr + len;
      continue;
    }
    const uint8_t tlen = (flags >> 3) & 7;
    uint16_t msglen = len - 2 - tlen;
    const uint16_t msgstart = (ptr + 2) & EVENTLOG_MASK;

    ts += evl_read_delta_ts(ef, ptr);

    size_t buflen = 0;
    buflen += print_timestamp_to_buf(linebuf, sizeof(linebuf), ts);

    if(buflen < sizeof(linebuf)) {
      buflen += snprintf(linebuf + buflen, sizeof(linebuf) - buflen,
                         "\t%s\t", level2str[level]);
    }

    msglen = MIN(msglen, sizeof(linebuf) - buflen - 1);

    const uint16_t msgend = (msgstart + msglen) & EVENTLOG_MASK;

    if(msgend >= msgstart) {
      memcpy(linebuf + buflen, ef->data + msgstart, msglen);
      buflen += msglen;
    } else {
      memcpy(linebuf + buflen, ef->data + msgstart, EVENTLOG_SIZE - msgstart);
      buflen += EVENTLOG_SIZE - msgstart;
      memcpy(linebuf + buflen, ef->data, msgend);
      buflen += msgend;
    }
    sf.f.ptr = ptr + len;
    mutex_unlock(&ef->mutex);
    linebuf[buflen++] = '\n';

    if(f == NULL) {
      error_t err =
        fs_open("log/log.txt", FS_CREAT | FS_WRONLY | FS_APPEND, &f);
      if(err) {
        evlog(LOG_ERR, "Unable to open logfile: %s", error_to_string(err));
        thread_exit(NULL);
      }
    }
    fs_write(f, linebuf, buflen);

    if(sync_deadline == 0)
      sync_deadline = clock_get() + 1000000;

    if(fs_size(f) > g_logfile_max_size) {
      fs_close(f);
      f = NULL;
      fs_rename("log/log.txt", "log/old.txt");
      evlog(LOG_INFO, "Log rotated");
    }

    mutex_lock(&ef->mutex);
  }
}


void
eventlog_to_fs(size_t max_file_size)
{
  g_logfile_max_size = max_file_size;
  int flags = TASK_DETACHED | TASK_NO_FPU | TASK_NO_DMA_STACK;
  thread_create(eventlog_to_fs_thread, NULL, 2048, "fslog", flags, 4);
}

#endif
