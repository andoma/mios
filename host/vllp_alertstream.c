#include "vllp_alertstream.h"

#include "vllp.h"

#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <sys/queue.h>

#define ALERT_PUB_BEGIN 1
#define ALERT_PUB_RAISE 2
#define ALERT_PUB_END 3

typedef struct vllp_alertstream {
  vllp_channel_t *vc;
  void *opaque;
  void (*mark)(void *opaque);
  void (*raise)(void *opaque,
                const char *key, int level, const char *msg);
  void (*sweep)(void *opaque);
} vllp_alertstream_t;


static void
vllp_alert_raise(vllp_alertstream_t *vas, const uint8_t *buf, size_t len)
{
  if(len < 2)
    return;

  int level = buf[0];
  int keylen = buf[1];

  buf += 2;
  len -= 2;
  if(keylen < 1 || keylen > len)
    return;

  char *key = malloc(keylen + 1);
  memcpy(key, buf, keylen);
  key[keylen] = 0;

  buf += keylen;
  len -= keylen;

  char *msg = malloc(len + 1);
  memcpy(msg, buf, len);
  msg[len] = 0;

  vas->raise(vas->opaque, key, level, msg);
  free(key);
  free(msg);
}

static void
vllp_alert_rx(void *opaque, const void *data, size_t len)
{
  vllp_alertstream_t *vas = opaque;

  const uint8_t *u8 = data;
  if(len < 1)
    return;

  int code = u8[0];
  switch(code) {
  case ALERT_PUB_BEGIN:
    vas->mark(vas->opaque);
    break;
  case ALERT_PUB_RAISE:
    vllp_alert_raise(vas, data + 1, len - 1);
    break;
  case ALERT_PUB_END:
    vas->sweep(vas->opaque);
    break;
  }
}

static void
vllp_alert_eof(void *opaque, int error)
{
}


vllp_alertstream_t *
vllp_alertstream_create(vllp_t *v, void *opaque,
                        void (*mark)(void *opaque),
                        void (*raise)(void *opaque, const char *key,
                                      int level, const char *msg),
                        void (*sweep)(void *opaque))
{
  vllp_alertstream_t *va = calloc(1, sizeof(vllp_alertstream_t));
  va->opaque = opaque;
  va->mark = mark;
  va->raise = raise;
  va->sweep = sweep;
  va->vc = vllp_channel_create(v, "alert", VLLP_CHANNEL_RECONNECT,
                               vllp_alert_rx, vllp_alert_eof, NULL, va);
  return va;
}


void
vllp_alertstream_destroy(vllp_alertstream_t *va)
{
  vllp_channel_close(va->vc, 0, 0);
  free(va);
}
