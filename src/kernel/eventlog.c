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



#ifdef ENABLE_NET_PCS

#include "net/pcs/pcs.h"
#include "net/pcs_shell.h"

typedef struct {
  follower_t f;
  pcs_t *pcs;
  pcs_iface_t *pi;
  uint64_t ts;
  uint32_t seq;
} evlog_pcs_follower_t;



static void
evlog_to_pcs_fill(void *arg, size_t avail,
                  void (*wrfn)(pcs_t *pcs, const void *buf,
                               size_t len))
{
  evlog_pcs_follower_t *epf = arg;
  evlogfifo_t *ef = &ef0;

  if(avail < 6)
    return;

  mutex_lock(&ef->mutex);

  if(wrfn == NULL) {
    // EOS
    LIST_REMOVE(&epf->f, link);
    free(epf);
  } else {

    uint16_t ptr = epf->f.ptr;

    if(ptr != ef->head) {

      if(ptr == ef->tail) {
        epf->ts = ef->ts_tail;
        epf->seq = ef->seq_tail;
        uint8_t hdr[2 + 4];
        hdr[0] = 6;
        hdr[1] = 0x80;
        hdr[2] = epf->seq;
        hdr[3] = epf->seq >> 8;
        hdr[4] = epf->seq >> 16;
        hdr[5] = epf->seq >> 24;
        wrfn(epf->pcs, hdr, 6);
        avail -= 6;
      }

      const uint8_t len = ef->data[(ptr + 0) & EVENTLOG_MASK];
      const uint8_t flags = ef->data[(ptr + 1) & EVENTLOG_MASK];
      const uint8_t level = flags & 7;
      const uint8_t tlen = (flags >> 3) & 7;
      const uint16_t msglen = len - 2 - tlen;

      if(msglen + 10 <= avail) {

        const uint16_t msgstart = (ptr + 2) & EVENTLOG_MASK;
        const uint16_t msgend = (msgstart + msglen) & EVENTLOG_MASK;

        epf->ts += evl_read_delta_ts(ef, ptr);

        uint64_t ms_ago = (clock_get() - epf->ts) / 1000;

        uint8_t hdr[2 + 8];
        int tslen = 0;
        for(uint32_t i = 0; i < 8; i++) {
          if(ms_ago == 0)
            break;
          hdr[2 + i] = ms_ago;
          ms_ago >>= 8;
          tslen++;
        }
        hdr[0] = 2 + tslen + msglen;
        hdr[1] = (tslen << 3) | level;
        pcs_t *pcs = epf->pcs;
        wrfn(pcs, hdr, tslen + 2);
        if(msgend >= msgstart) {
          wrfn(pcs, ef->data + msgstart, msglen);
        } else {
          wrfn(pcs, ef->data + msgstart, EVENTLOG_SIZE - msgstart);
          wrfn(pcs, ef->data, msgend);
        }
        wrfn(pcs, NULL, 0); // flush
        epf->f.ptr += len;
      }
    }
  }
  mutex_unlock(&ef->mutex);
}


static void
evlog_to_pcs_wakeup(follower_t *f)
{
  evlog_pcs_follower_t *epf = (evlog_pcs_follower_t *)f;
  pcs_iface_wakeup(epf->pi);
}


error_t
evlog_to_pcs(pcs_t *pcs)
{
  evlog_pcs_follower_t *epf =
    xalloc(sizeof(evlog_pcs_follower_t), 0, MEM_MAY_FAIL);

  if(epf == NULL)
    return ERR_NO_MEMORY;

  memset(epf, 0, sizeof(evlog_pcs_follower_t));
  epf->pcs = pcs;
  epf->pi = pcs_get_iface(pcs);

  evlogfifo_t *ef = &ef0;

  epf->f.ptr = ef->tail;
  epf->f.cb = evlog_to_pcs_wakeup;

  mutex_lock(&ef->mutex);
  LIST_INSERT_HEAD(&ef->followers, &epf->f, link);
  mutex_unlock(&ef->mutex);

  pcs_callback(pcs, epf, evlog_to_pcs_fill);
  return 0;
}

#endif

#endif
