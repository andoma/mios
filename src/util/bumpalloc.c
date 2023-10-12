#include <mios/bumpalloc.h>

#include <stdlib.h>
#include <malloc.h>
#include <string.h>


balloc_t *
balloc_create(size_t capacity)
{
  balloc_t *ba = xalloc(sizeof(balloc_t) + capacity, 0, MEM_MAY_FAIL);
  if(ba != NULL) {
    ba->capacity = capacity;
    ba->used = 0;
  }
  return ba;
}

void *
balloc_append_data(balloc_t *ba, const void *src, size_t srclen,
                   void **ptr, size_t *sizep)
{
  if(ba == NULL)
    return NULL;

  if(*ptr == NULL) {
    *ptr = ba->data + ba->used;
    // Null terminated string
    if(sizep == NULL) {
      if(ba->used + 1 >= ba->capacity)
        return NULL;
      ba->data[ba->used] = 0;
      ba->used++;
    } else {
      *sizep = 0;
    }
  }

  if(ba->used + srclen >= ba->capacity)
    return NULL;

  uint8_t *d = ba->data + ba->used - (sizep ? 0 : 1);
  memcpy(d, src, srclen);
  if(sizep) {
    *sizep = *sizep + srclen;
  } else {
    d[srclen] = 0;
  }

  ba->used += srclen;
  return d;
}


void *
balloc_alloc(balloc_t *ba, size_t size)
{
  if(ba == NULL)
    return NULL;

  ba->used = (ba->used + 3) & ~3;

  if(ba->used + size >= ba->capacity)
    return NULL;

  void *r = ba->data + ba->used;
  ba->used += size;
  return r;
}
