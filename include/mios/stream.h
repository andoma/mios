#pragma once

#include <stddef.h>

#define STREAM_READ_WAIT_NONE 0
#define STREAM_READ_WAIT_ONE  1
#define STREAM_READ_WAIT_ALL  2



typedef struct stream {
  int (*read)(struct stream *s, void *buf, size_t size, int wait);
  void (*write)(struct stream *s, const void *buf, size_t size);
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
