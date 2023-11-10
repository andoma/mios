#pragma once

#include <stddef.h>

#define STREAM_READ_WAIT_NONE 0
#define STREAM_READ_WAIT_ONE  1
#define STREAM_READ_WAIT_ALL  2

#define STREAM_WRITE_NO_WAIT  0x1
#define STREAM_WRITE_WAIT_DTR 0x2

typedef struct stream {

  __attribute__((access(write_only, 2, 3)))
  int (*read)(struct stream *s, void *buf, size_t size, int wait);

  __attribute__((access(read_only, 2, 3)))
  void (*write)(struct stream *s, const void *buf, size_t size, int flags);

} stream_t;



static inline int
stream_wait_is_done(int mode, size_t completed, size_t requested)
{
  switch(mode) {
  default:
    return 1;
  case STREAM_READ_WAIT_ONE:
    return !!completed;
  case STREAM_READ_WAIT_ALL:
    return completed == requested;
  }
}
