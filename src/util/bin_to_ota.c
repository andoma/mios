#include <mios/ota.h>

#include <mios/stream.h>
#include <mios/error.h>
#include <mios/block.h>
#include <mios/eventlog.h>

#include <sys/param.h>

#include <stdint.h>
#include <stddef.h>
#include <malloc.h>
#include <string.h>
#include <stdlib.h>

#include "crc32.h"

typedef struct {
  uint8_t magic[4];
  uint32_t size;
  uint32_t image_crc;
  uint32_t header_crc;
} otahdr_t;

typedef struct bin_to_ota {
  struct stream input;
  block_iface_t *output;
  uint32_t block;
  uint32_t offset;
  otahdr_t hdr;
} bin_to_ota_t;

static ssize_t
bto_write(struct stream *st, const void *data, size_t len, int flags)
{
  bin_to_ota_t *bto = (bin_to_ota_t *)st;
  error_t err;
  size_t olen = len;

  if(data == NULL) {
    if(bto->hdr.size == 0)
      return 0;

    // Flush
    bto->hdr.image_crc = ~bto->hdr.image_crc;

    evlog(LOG_INFO, "OTA Done Size:%d (CRC: 0x%08x)",
          bto->hdr.size, bto->hdr.image_crc);

    err = bto->output->erase(bto->output, 0);
    if(err)
      return err;

    memcpy(bto->hdr.magic, "OTA1", 4);
    bto->hdr.header_crc = ~crc32(0, &bto->hdr, sizeof(otahdr_t) - 4);
    err = bto->output->write(bto->output, 0, 0, &bto->hdr, sizeof(otahdr_t));
    if(err)
      return err;

    bto->output->ctrl(bto->output, BLOCK_SUSPEND);

    return 0;
  }

  while(len) {

    if(bto->offset == 0) {
      // New block, erase
      evlog(LOG_DEBUG, "OTA Erasing block %d", bto->block);
      err = bto->output->erase(bto->output, bto->block);
      if(err)
        return err;
    }

    size_t chunk = MIN(len, bto->output->block_size - bto->offset);
    ssize_t written = bto->output->write(bto->output, bto->block, bto->offset,
                                         data, chunk);
    if(written < 0)
      return written;

    bto->hdr.image_crc = crc32(bto->hdr.image_crc, data, chunk);
    bto->hdr.size += chunk;

    bto->offset += chunk;
    data += chunk;
    len -= chunk;

    if(bto->offset == bto->output->block_size) {
      bto->offset = 0;
      bto->block++;
    }
  }
  return olen;
}


static void
bto_close(struct stream *st)
{
  free(st);
}



static const stream_vtable_t bto_vtable = {
  .write = bto_write,
  .close = bto_close,
};

struct stream *
bin_to_ota(struct block_iface *output, uint32_t block_offset)
{
  if(output == NULL)
    return NULL;

  bin_to_ota_t *bto = xalloc(sizeof(bin_to_ota_t), 0,
                             MEM_MAY_FAIL | MEM_CLEAR);
  if(bto == NULL)
    return NULL;

  bto->input.vtable = &bto_vtable;
  bto->output = output;
  bto->block = block_offset;
  return &bto->input;
}
