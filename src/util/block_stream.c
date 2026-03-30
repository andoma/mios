#include <mios/block.h>
#include <mios/stream.h>

#include <malloc.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
  stream_t stream;
  block_iface_t *bi;
  size_t block;
  size_t offset;
  size_t total_written;
} block_write_stream_t;


static ssize_t
bws_write(stream_t *s, const void *buf, size_t size, int flags)
{
  block_write_stream_t *bws = (block_write_stream_t *)s;
  if(buf == NULL) {
    block_ctrl(bws->bi, BLOCK_SYNC);
    return 0;
  }

  const size_t bs = bws->bi->block_size;
  const uint8_t *src = buf;
  size_t remaining = size;

  while(remaining > 0) {
    // Erase block on first write to it
    if(bws->offset == 0) {
      error_t err = block_erase(bws->bi, bws->block, 1);
      if(err)
        return err;
    }

    size_t chunk = bs - bws->offset;
    if(chunk > remaining)
      chunk = remaining;

    error_t err = block_write(bws->bi, bws->block, bws->offset, src, chunk);
    if(err)
      return err;

    src += chunk;
    remaining -= chunk;
    bws->offset += chunk;
    bws->total_written += chunk;

    if(bws->offset >= bs) {
      bws->block++;
      bws->offset = 0;
    }
  }

  return size;
}


static void
bws_close(stream_t *s)
{
  free(s);
}


static const stream_vtable_t block_write_stream_vtable = {
  .write = bws_write,
  .close = bws_close,
};


stream_t *
block_write_stream_create(block_iface_t *bi)
{
  if(bi == NULL)
    return NULL;

  block_write_stream_t *bws = xalloc(sizeof(*bws), 0,
                                      MEM_MAY_FAIL | MEM_CLEAR);
  if(bws == NULL)
    return NULL;

  bws->stream.vtable = &block_write_stream_vtable;
  bws->bi = bi;
  return &bws->stream;
}
