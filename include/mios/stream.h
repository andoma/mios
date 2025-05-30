#pragma once

#include <stddef.h>
#include <sys/uio.h>

#include "poll.h"

struct stream;
struct task_waitable;

#define STREAM_WRITE_NO_WAIT  0x1  /* Write as much as possible, but don't
                                    * wait. Returns number of bytes written
                                    */
#define STREAM_WRITE_ALL      0x2  /* Write all or nothing.
                                    * Only relevant when combined with
                                    * STREAM_WRITE_NO_WAIT
                                    */

#define STREAM_WRITE_WAIT_DTR 0x4  /* Wait for DTR before writing
                                    * Only useful for serial ports
                                    * TODO: Odd API. Rework this somehow
                                    */

typedef struct stream_vtable {

  __attribute__((access(write_only, 2, 3)))
  ssize_t (*read)(struct stream *s, void *buf, size_t size, size_t required);

  __attribute__((access(read_only, 2, 3)))
  ssize_t (*write)(struct stream *s, const void *buf, size_t size, int flags);

  ssize_t (*writev)(struct stream *s, struct iovec *iov, size_t iovcnt,
                    int flags);

  void (*close)(struct stream *s);

  struct task_waitable *(*poll)(struct stream *s, poll_type_t type);

  ssize_t (*peek)(struct stream *s, void **buf, int wait);

  ssize_t (*drop)(struct stream *s, size_t bytes);

} stream_vtable_t;


typedef struct stream {
  const stream_vtable_t *vtable;
} stream_t;

__attribute__((always_inline))
static inline ssize_t
stream_read(struct stream *s, void *buf, size_t size, size_t required)
{
  return s->vtable->read(s, buf, size, required);
}

__attribute__((always_inline))
static inline ssize_t
stream_write(struct stream *s, const void *buf, size_t size, int flags)
{
  return s->vtable->write(s, buf, size, flags);
}

__attribute__((always_inline))
static inline ssize_t
stream_writev(struct stream *s, struct iovec *iov, size_t iovcnt, int flags)
{
  return s->vtable->writev(s, iov, iovcnt, flags);
}

__attribute__((always_inline))
static inline void
stream_close(struct stream *s)
{
  s->vtable->close(s);
}

__attribute__((always_inline))
static inline ssize_t
stream_peek(struct stream *s, void **buf, int wait)
{
  return s->vtable->peek(s, buf, wait);
}


__attribute__((always_inline))
static inline ssize_t
stream_drop(struct stream *s, size_t bytes)
{
  return s->vtable->drop(s, bytes);
}

__attribute__((always_inline))
static inline ssize_t
stream_flush(struct stream *s)
{
  return s->vtable->write(s, NULL, 0, 0);
}

__attribute__((always_inline))
static inline struct task_waitable *
stream_poll(struct stream *s, poll_type_t pt)
{
  return s->vtable->poll(s, pt);
}
