#pragma once

#include <stddef.h>
#include "poll.h"

#define STREAM_WRITE_NO_WAIT  0x1
#define STREAM_WRITE_WAIT_DTR 0x2

struct task_waitable;

typedef struct stream {

  __attribute__((access(write_only, 2, 3)))
  ssize_t (*read)(struct stream *s, void *buf, size_t size, size_t required);

  __attribute__((access(read_only, 2, 3)))
  ssize_t (*write)(struct stream *s, const void *buf, size_t size, int flags);

  void (*close)(struct stream *s);

  struct task_waitable *(*poll)(struct stream *s, poll_type_t type);

} stream_t;

