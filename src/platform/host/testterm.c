#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/param.h>

#include <mios/stream.h>
#include <mios/task.h>

#include "testterm.h"

#define TT_IN_SIZE  512
#define TT_OUT_SIZE 32768

struct testterm {
  stream_t st;

  mutex_t mutex;
  cond_t in_cond;    /* keystrokes available for the console side to read */
  cond_t out_cond;   /* the console wrote something the suite can inspect */

  uint8_t in[TT_IN_SIZE];
  size_t in_used;

  uint8_t out[TT_OUT_SIZE];
  size_t out_used;
  size_t out_dropped;
};


int
testterm_contains(const char *hay, const char *needle)
{
  const size_t n = strlen(needle);
  const size_t h = strlen(hay);
  if(n > h)
    return 0;
  for(size_t i = 0; i + n <= h; i++) {
    if(!memcmp(hay + i, needle, n))
      return 1;
  }
  return 0;
}


static ssize_t
tt_read(stream_t *s, void *buf, size_t size, size_t required)
{
  testterm_t *tt = (testterm_t *)s;
  uint8_t *u8 = buf;

  mutex_lock(&tt->mutex);
  size_t i = 0;
  while(i < size) {
    while(tt->in_used == 0) {
      if(i >= required) {
        mutex_unlock(&tt->mutex);
        return i;
      }
      cond_wait(&tt->in_cond, &tt->mutex);
    }
    size_t n = MIN(size - i, tt->in_used);
    memcpy(u8 + i, tt->in, n);
    memmove(tt->in, tt->in + n, tt->in_used - n);
    tt->in_used -= n;
    i += n;
  }
  mutex_unlock(&tt->mutex);
  return i;
}


static ssize_t
tt_write(stream_t *s, const void *buf, size_t size, int flags)
{
  testterm_t *tt = (testterm_t *)s;

  if(buf == NULL)
    return 0; /* flush */

  mutex_lock(&tt->mutex);
  size_t n = MIN(size, TT_OUT_SIZE - tt->out_used);
  memcpy(tt->out + tt->out_used, buf, n);
  tt->out_used += n;
  tt->out_dropped += size - n;
  cond_broadcast(&tt->out_cond);
  mutex_unlock(&tt->mutex);
  return size;
}


static task_waitable_t *
tt_poll(stream_t *s, poll_type_t type)
{
  testterm_t *tt = (testterm_t *)s;

  if(type == POLL_STREAM_WRITE)
    return NULL; /* never blocks */

  if(tt->in_used)
    return NULL;
  return &tt->in_cond;
}


static const stream_vtable_t tt_vtable = {
  .read = tt_read,
  .write = tt_write,
  .poll = tt_poll,
};


testterm_t *
testterm_create(void)
{
  testterm_t *tt = calloc(1, sizeof(testterm_t));
  tt->st.vtable = &tt_vtable;
  mutex_init(&tt->mutex, "ttmtx");
  cond_init(&tt->in_cond, "ttin");
  cond_init(&tt->out_cond, "ttout");
  return tt;
}


stream_t *
testterm_stream(testterm_t *tt)
{
  return &tt->st;
}


size_t
testterm_type(testterm_t *tt, const void *buf, size_t len)
{
  mutex_lock(&tt->mutex);
  size_t n = MIN(len, TT_IN_SIZE - tt->in_used);
  memcpy(tt->in + tt->in_used, buf, n);
  tt->in_used += n;
  if(n)
    cond_broadcast(&tt->in_cond);
  mutex_unlock(&tt->mutex);
  return n;
}


size_t
testterm_types(testterm_t *tt, const char *str)
{
  return testterm_type(tt, str, strlen(str));
}


void
testterm_out_clear(testterm_t *tt)
{
  mutex_lock(&tt->mutex);
  tt->out_used = 0;
  tt->out_dropped = 0;
  mutex_unlock(&tt->mutex);
}


size_t
testterm_out_len(testterm_t *tt)
{
  mutex_lock(&tt->mutex);
  size_t n = tt->out_used;
  mutex_unlock(&tt->mutex);
  return n;
}


size_t
testterm_out_get(testterm_t *tt, char *dst, size_t dstsize)
{
  mutex_lock(&tt->mutex);
  size_t n = MIN(tt->out_used, dstsize - 1);
  memcpy(dst, tt->out, n);
  dst[n] = 0;
  mutex_unlock(&tt->mutex);
  return n;
}


int
testterm_out_has(testterm_t *tt, const char *needle)
{
  static char snap[TT_OUT_SIZE + 1];   /* suites are single-threaded here */
  testterm_out_get(tt, snap, sizeof(snap));
  return testterm_contains(snap, needle);
}


int
testterm_out_wait(testterm_t *tt, const char *needle, uint64_t timeout)
{
  const uint64_t deadline = clock_get() + timeout;
  while(!testterm_out_has(tt, needle)) {
    if(clock_get() >= deadline)
      return 0;
    usleep(10000);
  }
  return 1;
}
