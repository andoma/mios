#include <mios/block.h>
#include <mios/task.h>
#include <mios/io.h>

#include <assert.h>
#include <unistd.h>
#include <malloc.h>
#include <stdio.h>

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

typedef struct xspi {
  block_iface_t iface;
  //  device_t dev;
  mutex_t mutex;
  uint32_t base;
  

} xspi_t;


#if 0

static error_t
cmd_xspi(cli_t *cli, int argc, char **argv)
{

  gpio_conf_af(GPIO_PN(1), 9, GPIO_PUSH_PULL, GPIO_SPEED_HIGH, GPIO_PULL_NONE);
  gpio_conf_af(GPIO_PN(2), 9, GPIO_PUSH_PULL, GPIO_SPEED_HIGH, GPIO_PULL_NONE);
  gpio_conf_af(GPIO_PN(3), 9, GPIO_PUSH_PULL, GPIO_SPEED_HIGH, GPIO_PULL_NONE);
  gpio_conf_af(GPIO_PN(6), 9, GPIO_PUSH_PULL, GPIO_SPEED_HIGH, GPIO_PULL_NONE);

  uint32_t base = XSPI2_BASE;
  clk_enable(CLK_XSPIM);
  clk_enable(CLK_XSPI2);
  cli_printf(cli, "base: 0x%x\n", base);

  reg_wr(base + XSPI_CR,
         (0b01 << 28) | // FMODE = READ
         1);            // Enable

  reg_wr(base + XSPI_DCR1,
         (20 << 16) | // devsize, made up
         (0b010 << 24) | // Standard mode
         0);

  reg_wr(base + XSPI_DCR2,
         32 | // prescaler
         0);

  reg_wr(base + XSPI_DLR, 256);

  reg_wr(base + XSPI_CCR,
         (0b001 << 24) | // Data on single line
         ( 0b10 << 12) | // 24bit address
         (0b001 << 8)  | // Address on single line
         (0b001 << 0)  | // Instruction on single line
         0);

  reg_wr(base + XSPI_IR,
         3); // 24bit READ

  reg_wr(base + XSPI_AR, 64);

  // Read back data, ugly, but shuold be sufficient to test
  int wrap = 0;
  while(1) {
    uint32_t sr = reg_rd(base + XSPI_SR);
    if(!(sr & (1 << 5)))
      break; // Not busy == done
    int flevel = (sr >> 8) & 0x7f;
    if(flevel == 0)
      continue;
    cli_printf(cli, "%02x%c", reg_rd8(base + XSPI_DR),
               (wrap & 7) == 7 ? '\n' : ' ');
    wrap++;
  }

  return 0;
}


CLI_CMD_DEF("xspi", cmd_xspi);
#endif


static error_t
xspi_erase(struct block_iface *bi, size_t block, size_t count)
{
  //  xspi_t *xs = (xspi_t *)bi;
  return ERR_NOT_IMPLEMENTED;
}

static error_t
xspi_write(struct block_iface *bi, size_t block,
           size_t offset, const void *data, size_t length)
{
  //  xspi_t *xs = (xspi_t *)bi;
  return ERR_NOT_IMPLEMENTED;
}

static error_t
xspi_read(struct block_iface *bi, size_t block,
          size_t offset, void *data, size_t length)
{
  xspi_t *xs = (xspi_t *)bi;
  uint32_t addr = xs->iface.block_size * block + offset;

  reg_wr(xs->base + XSPI_DLR, length - 1);

  reg_wr(xs->base + XSPI_CCR,
         (0b001 << 24) | // Data on single line
         ( 0b10 << 12) | // 24bit address
         (0b001 << 8)  | // Address on single line
         (0b001 << 0)  | // Instruction on single line
         0);

  reg_wr(xs->base + XSPI_IR,
         3); // 24bit READ

  reg_wr(xs->base + XSPI_AR, addr);

  uint8_t *dst = data;
  size_t i = 0;

  // Read back data, ugly, but shuold be sufficient to test
  while(1) {
    uint32_t sr = reg_rd(xs->base + XSPI_SR);
    if(!(sr & (1 << 5)))
      break; // Not busy == done
    int flevel = (sr >> 8) & 0x7f;
    if(flevel == 0)
      continue;
    dst[i] = reg_rd8(xs->base + XSPI_DR);
    i++;
    assert(i <= length);
  }
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
    return 0;

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
  xspi_t *xs = xalloc(sizeof(xspi_t), 0, MEM_MAY_FAIL);
  if(xs == NULL)
    return NULL;

  clk_enable(CLK_XSPI2);

  gpio_conf_af(GPIO_PN(1), 9, GPIO_PUSH_PULL, GPIO_SPEED_HIGH, GPIO_PULL_NONE);
  gpio_conf_af(GPIO_PN(2), 9, GPIO_PUSH_PULL, GPIO_SPEED_HIGH, GPIO_PULL_NONE);
  gpio_conf_af(GPIO_PN(3), 9, GPIO_PUSH_PULL, GPIO_SPEED_HIGH, GPIO_PULL_NONE);
  gpio_conf_af(GPIO_PN(6), 9, GPIO_PUSH_PULL, GPIO_SPEED_HIGH, GPIO_PULL_NONE);

  xs->base = XSPI2_BASE;

  reg_wr(xs->base + XSPI_CR,
         (0b01 << 28) | // FMODE = READ
         1);            // Enable

  reg_wr(xs->base + XSPI_DCR1,
         (20 << 16) | // devsize, made up
         (0b010 << 24) | // Standard mode
         0);

  reg_wr(xs->base + XSPI_DCR2,
         32 | // prescaler
         0);

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
