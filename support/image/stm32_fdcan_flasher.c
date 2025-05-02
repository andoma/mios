#include "stm32_fdcan_flasher.h"

#include "mios_image.h"

#include <string.h>
#include <stdio.h>
#include <sys/param.h>

typedef int (can_tx_t)(void *opaque,
                       uint32_t can_id,
                       const void *buf,
                       size_t len);

typedef int (can_rx_t)(void *opaque,
                       uint32_t can_id,
                       void *buf,
                       size_t len,
                       int timeout);

typedef struct {
  void *opaque;
  can_tx_t *tx;
  can_rx_t *rx;
  uint32_t base;
  char *errbuf;
  size_t errlen;
} stm32_fdcan_ctx_t;


static int
stm32_wait_ack(stm32_fdcan_ctx_t *ctx, uint32_t id, int timeout,
               const char *errinfo)
{
  uint8_t buf;
  int r = ctx->rx(ctx->opaque, id, &buf, 1, timeout);
  if(r < 0) {
    snprintf(ctx->errbuf, ctx->errlen,
             "%s: ACK not received (%s)", errinfo, strerror(-r));
    return -1;
  }
  if(buf != 0x79) {
    snprintf(ctx->errbuf, ctx->errlen,
             "%s: Expected ACK byte (0x79) got 0x%02x", errinfo, buf);
    return -1;
  }
  return 0;
}


static int
stm32_read_mem(stm32_fdcan_ctx_t *ctx, uint32_t addr, void *buf, size_t len)
{
  int r;
  uint8_t read_mem[5] = {
    addr >> 24,
    addr >> 16,
    addr >> 8,
    addr,
    len - 1};

  r = ctx->tx(ctx->opaque, ctx->base | 0x11, read_mem, sizeof(read_mem));
  if(r < 0) {
    snprintf(ctx->errbuf, ctx->errlen,
             "read-mem: Failed to send (%s)", strerror(-r));
    return -1;
  }
  if(stm32_wait_ack(ctx, 0x11, 500, "read-mem-setup"))
    return -1;

  r = ctx->rx(ctx->opaque, 0x11, buf, len, 500);
  if(r < 0) {
    snprintf(ctx->errbuf, ctx->errlen,
             "read-mem: Failed to read (%s)", strerror(-r));
    return -1;
  }

  if(r != len) {
    snprintf(ctx->errbuf, ctx->errlen,
             "read-mem: Incorrect number of bytes, got %d expected %zd",
             r, len);
    return -1;
  }

  return stm32_wait_ack(ctx, 0x11, 500, "read-mem-complete");
}


int
stm32_write_mem(stm32_fdcan_ctx_t *ctx, uint32_t addr, const void *buf, size_t len)
{
  int r;

  uint8_t write_mem[5] = {
    addr >> 24,
    addr >> 16,
    addr >> 8,
    addr,
    len - 1};

  r = ctx->tx(ctx->opaque, ctx->base | 0x31, write_mem, sizeof(write_mem));
  if(r < 0) {
    snprintf(ctx->errbuf, ctx->errlen,
             "write-mem: Failed to send (%s)", strerror(-r));
    return -1;
  }

  if(stm32_wait_ack(ctx, 0x31, 500, "write-mem-setup"))
    return -1;

  for(size_t i = 0; i < len; i += 64) {
    size_t chunksize = MIN(64, len - i);
    r = ctx->tx(ctx->opaque, ctx->base | 0x31, buf + i, chunksize);
    if(r < 0) {
      snprintf(ctx->errbuf, ctx->errlen,
               "write-mem: Failed to send (%s)", strerror(-r));
      return -1;
    }
  }

  return stm32_wait_ack(ctx, 0x31, 500, "write-mem-complete");
}


static int
stm32_chip_erase(stm32_fdcan_ctx_t *ctx)
{
  int r;
  uint8_t chip_erase[2] = {255,255};
  r = ctx->tx(ctx->opaque, ctx->base | 0x44, chip_erase, 2);
  if(r < 0) {
    snprintf(ctx->errbuf, ctx->errlen,
             "chip-erase: Failed to send (%s)", strerror(-r));
    return -1;
  }

  if(stm32_wait_ack(ctx, 0x44, 500, "chip-erase-start"))
    return -1;

  if(stm32_wait_ack(ctx, 0x44, 10000, "chip-erase-complete"))
    return -1;

  return 0;
}




int
stm32_fdcan_flasher(const struct mios_image *mi,
                    void *opaque, can_tx_t *tx, can_rx_t *rx,
                    void (*log)(void *opaque, const char *msg),
                    int force_flash,
                    uint32_t base,
                    char *errbuf, size_t errlen)
{
  stm32_fdcan_ctx_t ctx = {opaque, tx, rx, base, errbuf, errlen};
  char msgbuf[512];
  uint8_t buildid[20];

  if(stm32_read_mem(&ctx, mi->buildid_paddr, buildid, 20))
    return -1;

  snprintf(msgbuf, sizeof(msgbuf),
           "BuildId: Running:%02x%02x%02x%02x... Image:%02x%02x%02x%02x...",
           buildid[0],
           buildid[1],
           buildid[2],
           buildid[3],
           mi->buildid[0],
           mi->buildid[1],
           mi->buildid[2],
           mi->buildid[3]);

  log(opaque, msgbuf);

  if(!force_flash && !memcmp(mi->buildid, buildid, 20))
    return 0;

  log(opaque, "Erasing chip");

  if(stm32_chip_erase(&ctx))
    return -1;

  log(opaque, "Writing image");

  size_t siz = 128;
  for(size_t i = 0; i < mi->image_size; i += siz) {
    size_t size = MIN(siz, mi->image_size - i);
    if(stm32_write_mem(&ctx, mi->load_addr + i, mi->image + i, size))
      return -1;
  }

  log(opaque, "Image written");

  return 0;
}
