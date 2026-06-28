#pragma once

// Pure scrollback ring buffer for vcon. No MIOS dependencies so it can be
// unit-tested on the host. Drop-oldest semantics: the buffer always holds the
// most recent min(total-written, size) bytes. Reads are non-destructive (a
// snapshot), since a console may be replayed to many clients over its life.

#include <stddef.h>
#include <stdint.h>
#include <string.h>

typedef struct vcon_ring {
  size_t size;       // capacity, 0 disables the ring
  size_t head;       // index of next byte to write
  size_t used;       // valid bytes, <= size
  uint8_t buf[0];    // storage; lives in the embedding allocation's tail
} vcon_ring_t;


// Append, dropping oldest bytes when full.
static inline void
vcon_ring_append(vcon_ring_t *r, const uint8_t *u8, size_t len)
{
  if(r->size == 0 || len == 0)
    return;

  if(len >= r->size) {
    // Only the last `size` bytes survive; lay them out linearly from 0.
    memcpy(r->buf, u8 + (len - r->size), r->size);
    r->head = 0;
    r->used = r->size;
    return;
  }

  size_t first = r->size - r->head;
  if(first > len)
    first = len;
  memcpy(r->buf + r->head, u8, first);
  if(len - first)
    memcpy(r->buf, u8 + first, len - first);

  r->head = (r->head + len) % r->size;
  r->used += len;
  if(r->used > r->size)
    r->used = r->size;
}


// Copy n bytes into dst, starting `back` bytes behind head (toward oldest) and
// proceeding toward newest. Caller guarantees n <= back <= used.
static inline void
vcon_ring_copy(const vcon_ring_t *r, size_t back, uint8_t *dst, size_t n)
{
  if(r->size == 0 || n == 0)
    return;

  size_t start = (r->head + r->size - back) % r->size;
  size_t first = r->size - start;
  if(first > n)
    first = n;
  memcpy(dst, r->buf + start, first);
  if(n - first)
    memcpy(dst + first, r->buf, n - first);
}


// Return the valid contents oldest-first as up to two contiguous segments.
// Either length may be zero. Avoids materializing a linear copy (the buffer
// can be several KB and the MCU has a strict stack-frame limit).
static inline void
vcon_ring_segments(const vcon_ring_t *r,
                   const uint8_t **p0, size_t *l0,
                   const uint8_t **p1, size_t *l1)
{
  if(r->size == 0 || r->used == 0) {
    *p0 = r->buf; *l0 = 0;
    *p1 = r->buf; *l1 = 0;
    return;
  }

  size_t start = (r->head + r->size - r->used) % r->size;
  size_t first = r->size - start;
  if(first > r->used)
    first = r->used;

  *p0 = r->buf + start; *l0 = first;
  *p1 = r->buf;         *l1 = r->used - first;
}
