#include <mios/block.h>
#include <mios/task.h>
#include <mios/io.h>

#include <assert.h>
#include <unistd.h>
#include <malloc.h>
#include <stdio.h>
#include <string.h>

#include "stm32n6_clk.h"
#include "stm32n6_pwr.h"


#define XSPI1_BASE 0x58025000
#define XSPI2_BASE 0x5802A000

#define XSPI_CR   0x000
#define XSPI_DCR1 0x008
#define XSPI_DCR2 0x00c
#define XSPI_SR   0x020
#define XSPI_DLR  0x040
#define XSPI_AR   0x048
#define XSPI_DR   0x050
#define XSPI_CCR  0x100
#define XSPI_IR   0x110

#define XSPI_STATE_IDLE 0
#define XSPI_STATE_BUSY 1

typedef struct xspi {
  block_iface_t iface;
  mutex_t mutex;
  uint32_t base;
  int64_t busy_until;
  uint8_t state;
} xspi_t;




static void
wait_busy(xspi_t *xs)
{
  while(1) {
    uint32_t sr = reg_rd(xs->base + XSPI_SR);
    if(!(sr & (1 << 5)))
      break; // Not busy == done
  }
}


static int
xspi_get_status(xspi_t *xs)
{
  reg_wr(xs->base + XSPI_CR,
         (0b01 << 28) | // FMODE = READ
         1);            // Enable

  reg_wr(xs->base + XSPI_DCR1,
         (31 << 16) | // devsize, made up
         (0b010 << 24) | // Standard mode
         0);

  reg_wr(xs->base + XSPI_DCR2,
         32 | // prescaler
         0);

  reg_wr(xs->base + XSPI_DLR, 0); // Read one byte

  reg_wr(xs->base + XSPI_CCR,
         (0b001 << 24) | // Data on single line
         (0b000 << 8)  | // No address
         (0b001 << 0)  | // Instruction on single line
         0);

  reg_wr(xs->base + XSPI_IR, 0x05); // Get Status

  int status = reg_rd8(xs->base + XSPI_DR);
  wait_busy(xs);
  return status;
}

static error_t
xspi_wait_ready(xspi_t *xs)
{
  if(xs->state == XSPI_STATE_IDLE)
    return 0;

  if(xs->busy_until) {
    sleep_until(xs->busy_until);
    xs->busy_until = 0;
  }

  for(int i = 0; i < 500; i++) {
    int status = xspi_get_status(xs);
    if((status & 1) == 0) {
      xs->state = XSPI_STATE_IDLE;
      return 0;
    }
    usleep(1000);
  }
  return ERR_FLASH_TIMEOUT;
}


static error_t
xspi_cmd(xspi_t *xs, uint8_t cmd)
{
  reg_wr(xs->base + XSPI_CR,
         (0b00 << 28) | // FMODE = WRITE
         1);            // Enable

  reg_wr(xs->base + XSPI_DCR1,
         (31 << 16) | // devsize, made up
         (0b010 << 24) | // Standard mode
         0);

  reg_wr(xs->base + XSPI_DCR2,
         32 | // prescaler
         0);

  reg_wr(xs->base + XSPI_DLR, 0);

  reg_wr(xs->base + XSPI_CCR,
         (0b000 << 24) | // No data
         (0b000 << 8)  | // No address
         (0b001 << 0)  | // Instruction on single line
         0);

  reg_wr(xs->base + XSPI_IR, cmd);
  wait_busy(xs);
  return 0;
}


static error_t
xspi_erase(struct block_iface *bi, size_t block, size_t count)
{
  xspi_t *xs = (xspi_t *)bi;

  xspi_wait_ready(xs);

  xspi_cmd(xs, 6); // Write enable

  uint32_t addr = xs->iface.block_size * block;

  reg_wr(xs->base + XSPI_CR,
         (0b00 << 28) | // FMODE = WRITE
         1);            // Enable

  reg_wr(xs->base + XSPI_DCR1,
         (31 << 16) | // devsize, made up
         (0b010 << 24) | // Standard mode
         0);

  reg_wr(xs->base + XSPI_DCR2,
         32 | // prescaler
         0);

  reg_wr(xs->base + XSPI_DLR, 0);

  reg_wr(xs->base + XSPI_CCR,
         (0b000 << 24) | // No data
         ( 0b10 << 12) | // 24bit address
         (0b001 << 8)  | // Address on single line
         (0b001 << 0)  | // Instruction on single line
         0);

  reg_wr(xs->base + XSPI_IR, 0x20); // 24bit ERASE

  reg_wr(xs->base + XSPI_AR, addr);

  wait_busy(xs);

  xs->state = XSPI_STATE_BUSY;
  xs->busy_until = clock_get() + 40000; // ~40ms typical 4KB erase

  return 0;
}

static error_t
xspi_write(struct block_iface *bi, size_t block,
           size_t offset, const void *data, size_t length)
{
  xspi_t *xs = (xspi_t *)bi;

  const size_t page_size = 256;

  while(length) {

    size_t to_copy = length;

    if(to_copy > page_size)
      to_copy = page_size;

    size_t last_byte = offset + to_copy - 1;
    if((last_byte & ~(page_size - 1)) != (offset & ~(page_size - 1))) {
      to_copy = page_size - (offset & (page_size - 1));
    }

    xspi_wait_ready(xs);

    xspi_cmd(xs, 6); // Write enable

    uint32_t addr = xs->iface.block_size * block + offset;

    reg_wr(xs->base + XSPI_CR,
           (0b00 << 28) | // FMODE = WRITE
           1);            // Enable

    reg_wr(xs->base + XSPI_DCR1,
           (31 << 16) | // devsize, made up
           (0b010 << 24) | // Standard mode
           0);

    reg_wr(xs->base + XSPI_DCR2,
           32 | // prescaler
           0);

    reg_wr(xs->base + XSPI_DLR, to_copy - 1);
    reg_wr(xs->base + XSPI_CCR,
           (0b001 << 24) | // Data on single line
           ( 0b10 << 12) | // 24bit address
           (0b001 << 8)  | // Address on single line
           (0b001 << 0)  | // Instruction on single line
           0);

    reg_wr(xs->base + XSPI_IR, 0x2);

    reg_wr(xs->base + XSPI_AR, addr);

    const uint8_t *src = data;
    size_t i = 0;

    for(; i + 4 <= to_copy; i += 4) {
      uint32_t w;
      memcpy(&w, &src[i], 4);
      reg_wr(xs->base + XSPI_DR, w);
    }
    for(; i < to_copy; i++) {
      reg_wr8(xs->base + XSPI_DR, src[i]);
    }
    wait_busy(xs);

    xs->state = XSPI_STATE_BUSY;
    xs->busy_until = clock_get() + 500; // ~500us typical page program

    length -= to_copy;
    data += to_copy;
    offset += to_copy;
  }
  return 0;
}

static error_t
xspi_read(struct block_iface *bi, size_t block,
          size_t offset, void *data, size_t length)
{
  xspi_t *xs = (xspi_t *)bi;

  error_t err = xspi_wait_ready(xs);
  if(err)
    return err;

  uint32_t addr = xs->iface.block_size * block + offset;

  reg_wr(xs->base + XSPI_CR,
         (0b01 << 28) | // FMODE = READ
         1);            // Enable

  reg_wr(xs->base + XSPI_DCR1,
         (31 << 16) | // devsize, made up
         (0b010 << 24) | // Standard mode
         0);

  reg_wr(xs->base + XSPI_DCR2,
         32 | // prescaler
         0);
  reg_wr(xs->base + XSPI_DLR, length - 1);

  reg_wr(xs->base + XSPI_CCR,
         (0b001 << 24) | // Data on single line
         ( 0b10 << 12) | // 24bit address
         (0b001 << 8)  | // Address on single line
         (0b001 << 0)  | // Instruction on single line
         0);

  reg_wr(xs->base + XSPI_IR, 0x03); // 24bit READ

  reg_wr(xs->base + XSPI_AR, addr);

  uint8_t *dst = data;
  size_t remaining = length;

  while(remaining > 0) {
    uint32_t sr = reg_rd(xs->base + XSPI_SR);
    int flevel = (sr >> 8) & 0x7f;

    if(flevel == 0) {
      if(!(sr & (1 << 5)))
        break; // Not busy and FIFO empty = transfer done
      continue;
    }

    while(flevel >= 4 && remaining >= 4) {
      uint32_t w = reg_rd(xs->base + XSPI_DR);
      memcpy(dst, &w, 4);
      dst += 4;
      remaining -= 4;
      flevel -= 4;
    }

    while(flevel > 0 && remaining > 0) {
      *dst++ = reg_rd8(xs->base + XSPI_DR);
      remaining--;
      flevel--;
    }
  }
  wait_busy(xs);
  return 0;
}


static error_t
xspi_ctrl(struct block_iface *bi, block_ctrl_op_t op)
{
  xspi_t *xs = (xspi_t *)bi;

  switch(op) {

  case BLOCK_LOCK:
    mutex_lock(&xs->mutex);
    return 0;

  case BLOCK_UNLOCK:
    mutex_unlock(&xs->mutex);
    return 0;

  case BLOCK_SYNC:
    return xspi_wait_ready(xs);

  case BLOCK_SUSPEND:
  case BLOCK_SHUTDOWN:
    return 0;

  default:
    return ERR_OPERATION_FAILED;
  }
}


block_iface_t *
xspi_norflash_create(void)
{
  xspi_t *xs = xalloc(sizeof(xspi_t), 0, MEM_MAY_FAIL | MEM_CLEAR);
  if(xs == NULL)
    return NULL;

  clk_enable(CLK_XSPI2);

  gpio_conf_af(GPIO_PN(1), 9, GPIO_PUSH_PULL, GPIO_SPEED_HIGH, GPIO_PULL_NONE);
  gpio_conf_af(GPIO_PN(2), 9, GPIO_PUSH_PULL, GPIO_SPEED_HIGH, GPIO_PULL_NONE);
  gpio_conf_af(GPIO_PN(3), 9, GPIO_PUSH_PULL, GPIO_SPEED_HIGH, GPIO_PULL_NONE);
  gpio_conf_af(GPIO_PN(6), 9, GPIO_PUSH_PULL, GPIO_SPEED_HIGH, GPIO_PULL_NONE);

  xs->base = XSPI2_BASE;

  mutex_init(&xs->mutex, "xspi");
  xs->iface.erase = xspi_erase;
  xs->iface.write = xspi_write;
  xs->iface.read = xspi_read;
  xs->iface.ctrl = xspi_ctrl;

  xs->iface.num_blocks = 4096; // probe? IDK
  xs->iface.block_size = 4096;

  return &xs->iface;
}


static block_iface_t *g_xspi2;

#include <mios/cli.h>
#include <stdlib.h>
#include <string.h>

static error_t
cmd_rdblk(cli_t *cli, int argc, char **argv)
{
  if(argc != 2)
    return ERR_INVALID_ARGS;

  if(g_xspi2 == NULL)
    g_xspi2 = xspi_norflash_create();

  size_t len = 4096;

  void *buf = xalloc(len, 0, MEM_MAY_FAIL);
  if(buf == NULL)
    return ERR_NO_MEMORY;

  memset(buf, 0x55, len);

  int blk = atoi(argv[1]);
  block_read(g_xspi2, blk, 0, buf, len);

  sthexdump(cli->cl_stream, "BLK", buf, len, blk * 4096);
  free(buf);
  return 0;
}

CLI_CMD_DEF("rdblk", cmd_rdblk);



static error_t
cmd_erblk(cli_t *cli, int argc, char **argv)
{
  if(argc != 2)
    return ERR_INVALID_ARGS;

  if(g_xspi2 == NULL)
    g_xspi2 = xspi_norflash_create();

  int blk = atoi(argv[1]);
  block_erase(g_xspi2, blk, 1);
  return 0;
}

CLI_CMD_DEF("erblk", cmd_erblk);

static error_t
cmd_wrblk(cli_t *cli, int argc, char **argv)
{
  if(argc != 2)
    return ERR_INVALID_ARGS;

  if(g_xspi2 == NULL)
    g_xspi2 = xspi_norflash_create();

  size_t len = 4096;

  void *buf = xalloc(len, 0, MEM_MAY_FAIL);
  if(buf == NULL)
    return ERR_NO_MEMORY;

  uint8_t *u8 = buf;
  for(int i = 0; i < len; i++)
    u8[i] = i + 0x80;

  int blk = atoi(argv[1]);
  block_write(g_xspi2, blk, 0, buf, len);
  free(buf);
  return 0;
}

CLI_CMD_DEF("wrblk", cmd_wrblk);


static error_t
cmd_flashtest(cli_t *cli, int argc, char **argv)
{
  if(g_xspi2 == NULL)
    g_xspi2 = xspi_norflash_create();
  if(g_xspi2 == NULL)
    return ERR_OPERATION_FAILED;

  block_iface_t *bi = g_xspi2;
  size_t bs = bi->block_size;

  uint8_t *wbuf = xalloc(bs, 0, MEM_MAY_FAIL);
  uint8_t *rbuf = xalloc(bs, 0, MEM_MAY_FAIL);
  if(!wbuf || !rbuf) {
    free(wbuf); free(rbuf);
    return ERR_NO_MEMORY;
  }

  static const int test_blocks[] = { 0, 1, 2, 10, 100 };
  error_t err = 0;

  for(size_t t = 0; t < sizeof(test_blocks)/sizeof(test_blocks[0]); t++) {
    int blk = test_blocks[t];
    cli_printf(cli, "--- Block %d (addr 0x%x) ---\n", blk, blk * (int)bs);

    // 1. Erase
    err = block_erase(bi, blk, 1);
    if(err) { cli_printf(cli, "  ERASE FAIL: %d\n", err); break; }

    // 2. Read back, verify 0xFF
    memset(rbuf, 0x55, bs);
    err = block_read(bi, blk, 0, rbuf, bs);
    if(err) { cli_printf(cli, "  READ FAIL: %d\n", err); break; }

    int bad = 0;
    for(size_t i = 0; i < bs; i++)
      if(rbuf[i] != 0xff) bad++;
    if(bad) {
      cli_printf(cli, "  ERASE VERIFY FAIL: %d bad bytes\n", bad);
      cli_printf(cli, "  First 32: ");
      for(int i = 0; i < 32; i++) cli_printf(cli, "%02x ", rbuf[i]);
      cli_printf(cli, "\n");
      err = ERR_MISMATCH; break;
    }
    cli_printf(cli, "  erase OK\n");

    // 3. Write various sizes
    static const size_t write_sizes[] = { 1, 7, 32, 64, 128, 256 };
    for(size_t ws = 0; ws < sizeof(write_sizes)/sizeof(write_sizes[0]); ws++) {
      size_t wlen = write_sizes[ws];
      size_t woff = ws * 256;  // Different offset for each size

      for(size_t i = 0; i < wlen; i++)
        wbuf[i] = (blk + i + wlen) & 0xff;

      err = block_write(bi, blk, woff, wbuf, wlen);
      if(err) {
        cli_printf(cli, "  WRITE(%d@%d) FAIL: %d\n", (int)wlen, (int)woff, err);
        goto done;
      }

      memset(rbuf, 0x55, wlen);
      err = block_read(bi, blk, woff, rbuf, wlen);
      if(err) {
        cli_printf(cli, "  READBACK(%d@%d) FAIL: %d\n", (int)wlen, (int)woff, err);
        goto done;
      }

      bad = 0;
      int first_bad = -1;
      for(size_t i = 0; i < wlen; i++) {
        if(rbuf[i] != wbuf[i]) {
          if(first_bad < 0) first_bad = i;
          bad++;
        }
      }
      if(bad) {
        cli_printf(cli, "  VERIFY(%d@%d) FAIL: %d mismatches, first at %d\n",
                   (int)wlen, (int)woff, bad, first_bad);
        cli_printf(cli, "    exp: ");
        for(int i = first_bad; i < first_bad + 16 && i < (int)wlen; i++)
          cli_printf(cli, "%02x ", wbuf[i]);
        cli_printf(cli, "\n    got: ");
        for(int i = first_bad; i < first_bad + 16 && i < (int)wlen; i++)
          cli_printf(cli, "%02x ", rbuf[i]);
        cli_printf(cli, "\n");
        err = ERR_MISMATCH;
        goto done;
      }
      cli_printf(cli, "  write %d bytes @ offset %d OK\n", (int)wlen, (int)woff);
    }

    // 4. Page boundary crossing writes
    err = block_erase(bi, blk, 1);
    if(err) { cli_printf(cli, "  RE-ERASE FAIL (boundary)\n"); break; }

    static const struct { size_t off; size_t len; } boundary_tests[] = {
      { 192, 128 },   // Crosses one page boundary (192-319)
      { 128, 512 },   // Crosses two page boundaries (128-639)
      { 250,  12 },   // Small write crossing boundary (250-261)
      { 4000, 96 },   // Near end of block (4000-4095)
    };

    for(size_t bt = 0; bt < sizeof(boundary_tests)/sizeof(boundary_tests[0]); bt++) {
      size_t woff = boundary_tests[bt].off;
      size_t wlen = boundary_tests[bt].len;

      for(size_t i = 0; i < wlen; i++)
        wbuf[i] = (blk + i + woff) & 0xff;

      err = block_write(bi, blk, woff, wbuf, wlen);
      if(err) {
        cli_printf(cli, "  BOUNDARY WRITE(%d@%d) FAIL: %d\n",
                   (int)wlen, (int)woff, err);
        goto done;
      }

      memset(rbuf, 0x55, wlen);
      err = block_read(bi, blk, woff, rbuf, wlen);
      if(err) {
        cli_printf(cli, "  BOUNDARY READ(%d@%d) FAIL: %d\n",
                   (int)wlen, (int)woff, err);
        goto done;
      }

      bad = 0;
      int first_bad = -1;
      for(size_t i = 0; i < wlen; i++) {
        if(rbuf[i] != wbuf[i]) {
          if(first_bad < 0) first_bad = i;
          bad++;
        }
      }
      if(bad) {
        cli_printf(cli, "  BOUNDARY VERIFY(%d@%d) FAIL: %d mismatches, first at %d\n",
                   (int)wlen, (int)woff, bad, first_bad);
        err = ERR_MISMATCH;
        goto done;
      }
      cli_printf(cli, "  boundary write %d bytes @ offset %d OK\n",
                 (int)wlen, (int)woff);
    }

    // 5. Rapid erase/write/read cycles (simulates LittleFS pattern)
    for(int r = 0; r < 5; r++) {
      err = block_erase(bi, blk, 1);
      if(err) { cli_printf(cli, "  RAPID ERASE FAIL iter %d\n", r); goto done; }

      // Write 32 bytes at offset 0 (metadata-like)
      for(size_t i = 0; i < 32; i++)
        wbuf[i] = (r * 17 + i + blk) & 0xff;
      err = block_write(bi, blk, 0, wbuf, 32);
      if(err) { cli_printf(cli, "  RAPID WRITE FAIL iter %d\n", r); goto done; }

      // Immediate small read (no printf between write and read)
      memset(rbuf, 0x55, 32);
      err = block_read(bi, blk, 0, rbuf, 32);
      if(err) { cli_printf(cli, "  RAPID READ FAIL iter %d\n", r); goto done; }

      bad = 0;
      for(size_t i = 0; i < 32; i++)
        if(rbuf[i] != wbuf[i]) bad++;
      if(bad) {
        cli_printf(cli, "  RAPID VERIFY FAIL iter %d: %d mismatches\n", r, bad);
        err = ERR_MISMATCH; goto done;
      }
    }
    cli_printf(cli, "  rapid erase/write/read OK\n");

    // 6. Full block write+verify
    err = block_erase(bi, blk, 1);
    if(err) { cli_printf(cli, "  RE-ERASE FAIL\n"); break; }

    for(size_t i = 0; i < bs; i++)
      wbuf[i] = (i * 7 + blk) & 0xff;

    err = block_write(bi, blk, 0, wbuf, bs);
    if(err) { cli_printf(cli, "  FULL WRITE FAIL\n"); break; }

    memset(rbuf, 0x55, bs);
    err = block_read(bi, blk, 0, rbuf, bs);
    if(err) { cli_printf(cli, "  FULL READ FAIL\n"); break; }

    bad = 0;
    for(size_t i = 0; i < bs; i++)
      if(rbuf[i] != wbuf[i]) bad++;
    if(bad) {
      cli_printf(cli, "  FULL VERIFY FAIL: %d mismatches\n", bad);
      err = ERR_MISMATCH; break;
    }
    cli_printf(cli, "  full block write+verify OK\n");

    // Cleanup
    block_erase(bi, blk, 1);
  }

done:
  free(wbuf);
  free(rbuf);
  if(!err)
    cli_printf(cli, "ALL TESTS PASSED\n");
  return err;
}

CLI_CMD_DEF("flashtest", cmd_flashtest);

