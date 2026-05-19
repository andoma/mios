#include "stm32_fdcan_flasher.h"

#include "mios_image.h"

#include <string.h>
#include <stdio.h>
#include <sys/param.h>

/*
 * https://www.st.com/resource/en/application_note/an5405-how-to-use-fdcan-bootloader-protocol-on-stm32-mcus-stmicroelectronics.pdf
 */

typedef int (can_tx_t)(void *opaque,
                       uint32_t can_id,
                       const void *buf,
                       size_t len);

// rx is expected to compare against the received CAN id masked with 0xff.
// This is because the H7 FDCAN bootloader replies ACK on the bare command
// id (e.g. 0x11) but NACK on the host's TX id (base | cmd, e.g. 0x511),
// and we need to receive both.
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


int
stm32_get(stm32_fdcan_ctx_t *ctx, uint8_t *cmds, int cmds_max,
          int *n_cmds_out, int *proto_ver_out)
{
  int r;

  r = ctx->tx(ctx->opaque, ctx->base | 0x0, NULL, 0);
  if(r < 0) {
    snprintf(ctx->errbuf, ctx->errlen,
             "get: Failed to send (%s)", strerror(-r));
    return -1;
  }

  if(stm32_wait_ack(ctx, 0x0, 500, "get"))
    return -1;

  uint8_t b;
  r = ctx->rx(ctx->opaque, 0x0, &b, 1, 500);
  if(r != 1) {
    snprintf(ctx->errbuf, ctx->errlen,
             "get: Failed to read count (%d)", r);
    return -1;
  }
  int n_cmds = b;

  r = ctx->rx(ctx->opaque, 0x0, &b, 1, 500);
  if(r != 1) {
    snprintf(ctx->errbuf, ctx->errlen,
             "get: Failed to read protocol version (%d)", r);
    return -1;
  }
  if(proto_ver_out)
    *proto_ver_out = b;

  for(int i = 0; i < n_cmds; i++) {
    r = ctx->rx(ctx->opaque, 0x0, &b, 1, 500);
    if(r != 1) {
      snprintf(ctx->errbuf, ctx->errlen,
               "get: Failed to read opcode %d (%d)", i, r);
      return -1;
    }
    if(i < cmds_max)
      cmds[i] = b;
  }

  if(stm32_wait_ack(ctx, 0x0, 500, "get-complete"))
    return -1;

  *n_cmds_out = n_cmds;
  return 0;
}


static int
stm32_readout_unprotect(stm32_fdcan_ctx_t *ctx)
{
  int r;

  r = ctx->tx(ctx->opaque, ctx->base | 0x92, NULL, 0);
  if(r < 0) {
    snprintf(ctx->errbuf, ctx->errlen,
             "readout-unprotect: Failed to send (%s)", strerror(-r));
    return -1;
  }

  if(stm32_wait_ack(ctx, 0x92, 500, "readout-unprotect-start"))
    return -1;

  // Mass erase + OBL launch can easily take 20+ seconds on H7
  if(stm32_wait_ack(ctx, 0x92, 30000, "readout-unprotect-complete"))
    return -1;

  return 0;
}


int
stm32_get_id(stm32_fdcan_ctx_t *ctx)
{
  int r;

  r = ctx->tx(ctx->opaque, ctx->base | 0x2, NULL, 0);
  if(r < 0) {
    snprintf(ctx->errbuf, ctx->errlen,
             "get-id: Failed to send (%s)", strerror(-r));
    return -1;
  }

  if(stm32_wait_ack(ctx, 0x2, 500, "get-id"))
    return -1;

  uint8_t buf[2];
  r = ctx->rx(ctx->opaque, 0x2, buf, sizeof(buf), 500);
  if(r < 0) {
    snprintf(ctx->errbuf, ctx->errlen,
             "get-id: Failed to read (%s)", strerror(-r));
    return -1;
  }
  if(r != sizeof(buf)) {
    snprintf(ctx->errbuf, ctx->errlen,
             "get-id: Incorrect number of bytes, got %d expected %zd",
             r, sizeof(buf));
    return -1;
  }

  if(stm32_wait_ack(ctx, 0x2, 500, "get-id-complete"))
    return -1;

  return (buf[0] << 8) | buf[1];
}


static int
stm32_flash_erase(stm32_fdcan_ctx_t *ctx, uint32_t pages)
{
  int r;
  uint8_t req[2];

  if(pages >= 64)
    pages = 0xffff; // Chip erase

  req[0] = pages >> 8;
  req[1] = pages;

  r = ctx->tx(ctx->opaque, ctx->base | 0x44, req, sizeof(req));
  if(r < 0) {
    snprintf(ctx->errbuf, ctx->errlen,
             "flash-erase: Failed to send (%s)", strerror(-r));
    return -1;
  }

  if(stm32_wait_ack(ctx, 0x44, 500, "flash-erase-start"))
    return -1;

  if(pages != 0xffff) {

    if(stm32_wait_ack(ctx, 0x44, 500, "flash-erase-pagelist"))
      return -1;

    uint8_t pagelist[64] = {0};
    for(int i = 0; i <  pages; i++) {
      pagelist[i] = i;
    }

    r = ctx->tx(ctx->opaque, ctx->base | 0x44, pagelist, sizeof(pagelist));
    if(r < 0) {
      snprintf(ctx->errbuf, ctx->errlen,
               "flash-erase: Failed to send (%s)", strerror(-r));
      return -1;
    }
  }

  if(stm32_wait_ack(ctx, 0x44, 10000, "flash-erase-complete"))
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
  uint32_t flash_page_size;

  int chip_id = stm32_get_id(&ctx);
  if(chip_id < 0)
    return -1;

  snprintf(msgbuf, sizeof(msgbuf), "ChipId: 0x%04x", chip_id);
  log(opaque, msgbuf);

  switch(chip_id) {
  case 0x0483:  // stm32h723 ?
    flash_page_size = 128 * 1024;
    break;
  default:
    snprintf(errbuf, errlen, "Unsupported chip-id 0x%04x", chip_id);
    return -1;
  }

  // Log bootloader protocol version + command count (informational).
  // Note: the H7 FDCAN bootloader advertises the full command set in the
  // Get response regardless of RDP state, so we can't use it to detect
  // read protection. We fall back to a timeout-based check below.
  uint8_t cmds[16];
  int n_cmds = 0;
  int proto_ver = 0;
  if(stm32_get(&ctx, cmds, sizeof(cmds), &n_cmds, &proto_ver))
    return -1;

  snprintf(msgbuf, sizeof(msgbuf),
           "Bootloader proto 0x%02x, %d commands", proto_ver, n_cmds);
  log(opaque, msgbuf);

  if(stm32_read_mem(&ctx, mi->buildid_paddr, buildid, 20)) {
    // Read Memory failed. On STM32H7 under RDP != 0xAA the bootloader
    // silently bus-errors on user-flash reads -- no NACK, just no reply.
    // Confirm the bootloader is still responsive by re-issuing Get ID; if
    // it answers, the failure is read-protection and we recover by issuing
    // Readout Unprotect (0x92). If it doesn't answer, the link is broken
    // and we propagate the original read-mem error.
    char saved_err[256];
    snprintf(saved_err, sizeof(saved_err), "%s", errbuf);

    log(opaque, "Read Memory failed -- probing whether bootloader is alive");
    if(stm32_get_id(&ctx) < 0) {
      snprintf(errbuf, errlen, "%s", saved_err);
      return -1;
    }

    log(opaque, "Bootloader is alive -- chip is read-protected. "
                "Sending Readout Unprotect (full flash mass erase, "
                "may take up to 30s)");
    if(stm32_readout_unprotect(&ctx))
      return -1;

    // RDP is now 0xAA and the chip has OBL-launched (system reset). The
    // caller's retry loop reboots us into the bootloader on a fresh,
    // unprotected chip and the next pass flashes normally.
    snprintf(errbuf, errlen,
             "Chip was read-protected; RDP cleared, retry to flash");
    return -1;
  }

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

  uint32_t pages = (mi->image_size + flash_page_size - 1) / flash_page_size;

  snprintf(msgbuf, sizeof(msgbuf),
           "Erase %d pages", pages);
  log(opaque, msgbuf);

  if(stm32_flash_erase(&ctx, pages))
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
