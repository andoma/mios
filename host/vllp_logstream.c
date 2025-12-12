#include "vllp_logstream.h"

#include "vllp.h"

#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>

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

  const uint8_t *u8 = data;
  if(len < 1)
    return;

  int level = u8[0] & 7;
  int discontinuity = u8[0] & 0x40;
  int tsdeltasize = (u8[0] >> 3) & 7;

  u8++;
  len--;

  if(discontinuity) {
    if(len < 4)
      return;


    memcpy(&vl->seq, u8, sizeof(uint32_t));
    u8 += 4;
    len -= 4;
  }
  vl->seq++;

  int64_t tsdelta = 0;
  for(int i = 0; i < tsdeltasize; i++) {
    if(len < 1)
      return;
    tsdelta |= *u8 << (i * 8);
    u8++;
    len--;
  }

  char *buf = alloca(len + 1);
  memcpy(buf, u8, len);
  buf[len] = 0;
  vl->cb(vl->opaque, level, vl->seq, tsdelta, buf);
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
