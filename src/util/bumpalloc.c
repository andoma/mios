#include <mios/bumpalloc.h>

#include <stdlib.h>
#include <malloc.h>
#include <string.h>

balloc_t *
balloc_create(size_t size)
{
  balloc_t *ba = xalloc(sizeof(balloc_t) + size, 0, MEM_MAY_FAIL);
  if(ba != NULL) {
    ba->size = size;
    ba->used = 0;
  }
  return ba;
}

error_t
balloc_append_data(balloc_t *ba, const char *src, size_t srclen,
                   void **ptr, size_t *sizep)
{
  if(ba == NULL)
    return ERR_NO_BUFFER;

  if(*ptr == NULL) {
    *ptr = ba->data + ba->used;
    // Null terminated string
    if(sizep == NULL) {
      if(ba->used + 1 >= ba->size)
        return ERR_NO_BUFFER;
      ba->data[ba->used] = 0;
      ba->used++;
    } else {
      *sizep = 0;
    }
  }

  if(ba->used + srclen >= ba->size)
    return ERR_NO_BUFFER;

  uint8_t *d = ba->data + ba->used - (sizep ? 0 : 1);
  memcpy(d, src, srclen);
  if(sizep) {
    *sizep = *sizep + srclen;
  } else {
    d[srclen] = 0;
  }

  ba->used += srclen;
  return 0;
}


void *
balloc_alloc(balloc_t *ba, size_t size)
{
  if(ba == NULL)
    return NULL;

  ba->used = (ba->used + 3) & ~3;

  if(ba->used + size >= ba->size)
    return NULL;

  void *r = ba->data + ba->used;
  ba->used += size;
  return r;
}
