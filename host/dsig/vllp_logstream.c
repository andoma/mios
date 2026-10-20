#include "vllp_logstream.h"

#include <string.h>

#ifndef VLLP_SIM
#include "vllp.h"

#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#endif


/**
 * Framing (see evlog_svc_pull() in mios src/kernel/eventlog.c)
 *
 *   rDTT_TLLL
 *                r = reserved
 *                D = Discontinuity
 *                T = Size of timestamp delta
 *                L = Log level (0 - 7)
 *
 *    [4 byte sequence, if D is set]
 *    [Timestamp Delta, up to 7 bytes]
 *    [Message]
 */
int
vllp_logstream_decode(const void *data, size_t len, uint32_t *seq,
                      int *level, int64_t *ms_ago,
                      const char **msg, size_t *msglen)
{
  const uint8_t *u8 = data;
  if(len < 1)
    return -1;

  *level = u8[0] & 7;
  int discontinuity = u8[0] & 0x40;
  int tsdeltasize = (u8[0] >> 3) & 7;

  u8++;
  len--;

  if(discontinuity) {
    if(len < 4)
      return -1;

    memcpy(seq, u8, sizeof(uint32_t));
    u8 += 4;
    len -= 4;
  }
  (*seq)++;

  int64_t tsdelta = 0;
  for(int i = 0; i < tsdeltasize; i++) {
    if(len < 1)
      return -1;
    tsdelta |= (int64_t)*u8 << (i * 8);
    u8++;
    len--;
  }

  *ms_ago = tsdelta;
  *msg = (const char *)u8;
  *msglen = len;
  return 0;
}

#ifndef VLLP_SIM

typedef struct vllp_logstream {
  vllp_channel_t *vc;
  void *opaque;
  void (*cb)(void *opaque,
             int level, uint32_t sequence,
             int64_t ms_ago, const char *msg);

  uint32_t seq;
} vllp_logstream_t;

static void
vllp_log_rx(void *opaque, const void *data, size_t len)
{
  vllp_logstream_t *vl = opaque;

  int level;
  int64_t ms_ago;
  const char *msg;
  size_t msglen;
  if(vllp_logstream_decode(data, len, &vl->seq, &level, &ms_ago,
                           &msg, &msglen))
    return;

  char *buf = alloca(msglen + 1);
  memcpy(buf, msg, msglen);
  buf[msglen] = 0;
  vl->cb(vl->opaque, level, vl->seq, ms_ago, buf);
}

static void
vllp_log_eof(void *opaque, int error)
{
}


vllp_logstream_t *
vllp_logstream_create(vllp_t *v, void *opaque,
                      void (*cb)(void *opaque,
                                 int level, uint32_t sequence,
                                 int64_t ms_ago, const char *msg))
{
  vllp_logstream_t *vl = calloc(1, sizeof(vllp_logstream_t));
  vl->opaque = opaque;
  vl->cb = cb;
  vl->vc = vllp_channel_create(v, "log", VLLP_CHANNEL_RECONNECT, vllp_log_rx,
                               vllp_log_eof, NULL, vl);

  return vl;
}


void
vllp_logstream_destroy(vllp_logstream_t *vl)
{
  vllp_channel_close(vl->vc, 0, 0);
  free(vl);
}

#endif
