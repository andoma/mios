#include <mios/block.h>
#include <mios/task.h>
#include <mios/io.h>

#include <assert.h>
#include <unistd.h>
#include <malloc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "stm32n6_clk.h"
#include "stm32n6_pwr.h"
#include "irq.h"


#define XSPI1_BASE 0x58025000
#define XSPI2_BASE 0x5802A000

#define XSPI_CR   0x000
#define XSPI_DCR1 0x008
#define XSPI_DCR2 0x00c
#define XSPI_SR   0x020
#define XSPI_FCR  0x024
#define XSPI_DLR  0x040
#define XSPI_AR   0x048
#define XSPI_DR   0x050
#define XSPI_TCR  0x108
#define XSPI_CCR  0x100
#define XSPI_IR   0x110

#define XSPI_STATE_IDLE 0
#define XSPI_STATE_BUSY 1

typedef struct xspi {
  block_iface_t iface;
  mutex_t mutex;
  task_waitable_t waitq;
  uint32_t base;
  int64_t busy_until;
  uint8_t state;
  uint8_t tcf;
  uint8_t devsize;   // log2 of device size, for DCR1
  uint8_t prescaler; // clock prescaler for DCR2

  struct {
    uint8_t size;   // log2 of erase size
    uint8_t cmd3;   // 3-byte address command (0xff = not supported)
    uint8_t cmd4;   // 4-byte address command (0xff = not supported)
    uint16_t delay; // erase time in ms
  } erase_commands[4];
} xspi_t;


// CR interrupt enable bits
#define XSPI_CR_TCIE (1 << 17)
#define XSPI_CR_FTIE (1 << 18)

// FIFO threshold: FTF fires when >= 16 bytes available (read) or free (write)
#define XSPI_FTHRES 15

static void
xspi_irq(void *arg)
{
  xspi_t *xs = arg;
  uint32_t cr = reg_rd(xs->base + XSPI_CR);
  reg_wr(xs->base + XSPI_CR, cr & ~(XSPI_CR_TCIE | XSPI_CR_FTIE));
  if(reg_rd(xs->base + XSPI_SR) & (1 << 1))
    xs->tcf = 1;
  task_wakeup_sched_locked(&xs->waitq, 0);
}


static void
xspi_wait_tc(xspi_t *xs)
{
  int q = irq_forbid(IRQ_LEVEL_SCHED);

  if(!(reg_rd(xs->base + XSPI_SR) & (1 << 1))) {
    // TCF not yet set — enable TCIE and sleep
    reg_wr(xs->base + XSPI_CR, reg_rd(xs->base + XSPI_CR) | XSPI_CR_TCIE);
    while(!xs->tcf) {
      task_sleep_sched_locked(&xs->waitq);
    }
  }
  xs->tcf = 0;

  // Clear TCF for next operation
  reg_wr(xs->base + XSPI_FCR, (1 << 1));

  irq_permit(q);
}


static uint32_t
xspi_read_sfdp(xspi_t *xs, uint32_t addr)
{
  reg_wr(xs->base + XSPI_CR,
         (0b01 << 28) | // FMODE = READ
         1);            // Enable

  reg_wr(xs->base + XSPI_DCR1,
         (31 << 16) | (0b010 << 24));

  reg_wr(xs->base + XSPI_DCR2, 32);

  reg_wr(xs->base + XSPI_DLR, 3); // Read 4 bytes

  reg_wr(xs->base + XSPI_TCR, 8); // 8 dummy cycles for SFDP

  reg_wr(xs->base + XSPI_CCR,
         (0b001 << 24) | // Data on single line
         ( 0b10 << 12) | // 24bit address
         (0b001 << 8)  | // Address on single line
         (0b001 << 0)  | // Instruction on single line
         0);

  reg_wr(xs->base + XSPI_IR, 0x5a); // SFDP read command

  reg_wr(xs->base + XSPI_AR, addr);

  xspi_wait_tc(xs);
  uint32_t r = reg_rd(xs->base + XSPI_DR);
  reg_wr(xs->base + XSPI_TCR, 0); // Reset dummy cycles
  return r;
}


static int
xspi_get_status(xspi_t *xs)
{
  reg_wr(xs->base + XSPI_CR,
         (0b01 << 28) | 1); // FMODE = READ, Enable

  reg_wr(xs->base + XSPI_DLR, 0); // Read one byte

  reg_wr(xs->base + XSPI_CCR,
         (0b001 << 24) | // Data on single line
         (0b001 << 0));  // Instruction on single line

  reg_wr(xs->base + XSPI_IR, 0x05); // RDSR

  xspi_wait_tc(xs);
  return reg_rd8(xs->base + XSPI_DR);
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
         (0b00 << 28) | 1); // FMODE = WRITE, Enable

  reg_wr(xs->base + XSPI_DLR, 0);

  reg_wr(xs->base + XSPI_CCR,
         (0b001 << 0)); // Instruction on single line

  reg_wr(xs->base + XSPI_IR, cmd);
  xspi_wait_tc(xs);
  return 0;
}


static error_t
xspi_erase(struct block_iface *bi, size_t block, size_t count)
{
  xspi_t *xs = (xspi_t *)bi;

  while(count) {
    error_t err = xspi_wait_ready(xs);
    if(err)
      return err;

    xspi_cmd(xs, 6); // Write enable

    const uint32_t addr = block << 12; // block_size is always 4096

    err = ERR_INVALID_ADDRESS;
    for(int i = 3; i >= 0; i--) {
      if(xs->erase_commands[i].size == 0)
        continue;

      size_t chunk = 1 << (xs->erase_commands[i].size - 12);
      if(chunk > count)
        continue;
      if(addr & ((1 << xs->erase_commands[i].size) - 1))
        continue;

      int use4byte = addr > 0xffffff;
      uint8_t cmd = use4byte ? xs->erase_commands[i].cmd4
                             : xs->erase_commands[i].cmd3;
      if(cmd == 0xff)
        continue;

      reg_wr(xs->base + XSPI_CR,
             (0b00 << 28) | 1); // FMODE = WRITE, Enable

      reg_wr(xs->base + XSPI_DLR, 0);

      reg_wr(xs->base + XSPI_CCR,
             (0b000 << 24) |                         // No data
             (use4byte ? (0b11 << 12) : (0b10 << 12)) | // Address size
             (0b001 << 8) |                           // Address on single line
             (0b001 << 0));                           // Instruction on single line

      reg_wr(xs->base + XSPI_IR, cmd);
      reg_wr(xs->base + XSPI_AR, addr);

      xspi_wait_tc(xs);

      xs->state = XSPI_STATE_BUSY;
      xs->busy_until = clock_get() + xs->erase_commands[i].delay * 1000;

      block += chunk;
      count -= chunk;
      err = 0;
      break;
    }

    if(err)
      return err;
  }
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
    int use4byte = addr > 0xffffff;

    reg_wr(xs->base + XSPI_CR,
           (0b00 << 28) |        // FMODE = WRITE
           (XSPI_FTHRES << 8) |  // FIFO threshold
           1);                    // Enable

    reg_wr(xs->base + XSPI_DLR, to_copy - 1);
    reg_wr(xs->base + XSPI_CCR,
           (0b001 << 24) |                          // Data on single line
           (use4byte ? (0b11 << 12) : (0b10 << 12)) | // Address size
           (0b001 << 8) |                            // Address on single line
           (0b001 << 0));                            // Instruction on single line

    reg_wr(xs->base + XSPI_IR, use4byte ? 0x12 : 0x02);

    reg_wr(xs->base + XSPI_AR, addr);

    const uint8_t *src = data;
    size_t wr_remaining = to_copy;

    while(wr_remaining >= 4) {
      int q = irq_forbid(IRQ_LEVEL_SCHED);
      if(!(reg_rd(xs->base + XSPI_SR) & (1 << 2))) {
        // FTF not set — FIFO full, sleep until room
        reg_wr(xs->base + XSPI_CR,
               reg_rd(xs->base + XSPI_CR) | XSPI_CR_FTIE);
        task_sleep_sched_locked(&xs->waitq);
        irq_permit(q);
        continue;
      }
      irq_permit(q);

      uint32_t w;
      memcpy(&w, src, 4);
      reg_wr(xs->base + XSPI_DR, w);
      src += 4;
      wr_remaining -= 4;
    }
    while(wr_remaining > 0) {
      reg_wr8(xs->base + XSPI_DR, *src++);
      wr_remaining--;
    }
    xspi_wait_tc(xs);

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
  int use4byte = addr > 0xffffff;

  reg_wr(xs->base + XSPI_CR,
         (0b01 << 28) |        // FMODE = READ
         (XSPI_FTHRES << 8) |  // FIFO threshold
         1);                    // Enable

  reg_wr(xs->base + XSPI_DLR, length - 1);

  reg_wr(xs->base + XSPI_CCR,
         (0b001 << 24) |                          // Data on single line
         (use4byte ? (0b11 << 12) : (0b10 << 12)) | // Address size
         (0b001 << 8) |                            // Address on single line
         (0b001 << 0));                            // Instruction on single line

  reg_wr(xs->base + XSPI_IR, use4byte ? 0x13 : 0x03);

  reg_wr(xs->base + XSPI_AR, addr);

  uint8_t *dst = data;
  size_t remaining = length;

  while(remaining > 0) {
    int q = irq_forbid(IRQ_LEVEL_SCHED);
    uint32_t sr = reg_rd(xs->base + XSPI_SR);
    int flevel = (sr >> 8) & 0x7f;

    if(flevel == 0) {
      if(!(sr & (1 << 5))) {
        irq_permit(q);
        break; // Not busy and FIFO empty = transfer done
      }
      // FIFO empty, transfer still active — sleep until data or complete
      reg_wr(xs->base + XSPI_CR,
             reg_rd(xs->base + XSPI_CR) | XSPI_CR_FTIE | XSPI_CR_TCIE);
      task_sleep_sched_locked(&xs->waitq);
      irq_permit(q);
      continue;
    }
    irq_permit(q);

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
  xspi_wait_tc(xs);
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


static const uint16_t erase_time_multipliers[4] = {1, 16, 128, 1000};

static int
calc_erase_time(uint32_t v)
{
  return (1 + (v & 0x1f)) * erase_time_multipliers[(v >> 5) & 3];
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
  xs->devsize = 31; // Default for SFDP probing

  mutex_init(&xs->mutex, "xspi");
  task_waitable_init(&xs->waitq, "xspi");

  irq_enable_fn_arg(171, IRQ_LEVEL_SCHED, xspi_irq, xs);

  printf("xspi%d: ", xs->base == XSPI1_BASE ? 1 : 2);

  uint32_t sfdp_sig = xspi_read_sfdp(xs, 0);
  if(sfdp_sig != 0x50444653) {
    printf("Invalid SFDP signature [0x%x]\n", sfdp_sig);
    goto bad;
  }

  uint32_t hdr2 = xspi_read_sfdp(xs, 4);
  int nph = (hdr2 >> 16) & 0xff;

  uint32_t ptpj = 0;
  uint32_t ptp4 = 0;
  for(int i = 0; i <= nph; i++) {
    uint32_t ph1 = xspi_read_sfdp(xs, 0x8 + i * 8);
    uint32_t ph2 = xspi_read_sfdp(xs, 0xc + i * 8);
    if((ph1 & 0xff) == 0)
      ptpj = ph2 & 0xffffff;
    if((ph1 & 0xff) == 0x84)
      ptp4 = ph2 & 0xffffff;
  }

  if(ptpj == 0) {
    printf("Missing Flash Parameters\n");
    goto bad;
  }

  uint32_t density = xspi_read_sfdp(xs, ptpj + 4);
  if(density & 0x80000000) {
    printf("Unsupported density 0x%x\n", density);
    goto bad;
  }

  uint32_t size = (density + 1) >> 3;
  uint8_t devsize = 0;
  for(uint32_t s = size; s > 1; s >>= 1)
    devsize++;
  xs->devsize = devsize;

  xs->iface.num_blocks = size >> 12;
  xs->iface.block_size = 4096;

  printf("%d kB (%zd sectors", (int)(size >> 10), xs->iface.num_blocks);
  if(devsize > 24)
    printf(", 32-bit addr");
  printf(")  ");

  // Parse erase commands from SFDP
  uint32_t w = xspi_read_sfdp(xs, ptpj + 0x1c);
  xs->erase_commands[0].size = w;
  xs->erase_commands[0].cmd3 = w >> 8;
  xs->erase_commands[1].size = w >> 16;
  xs->erase_commands[1].cmd3 = w >> 24;

  w = xspi_read_sfdp(xs, ptpj + 0x20);
  xs->erase_commands[2].size = w;
  xs->erase_commands[2].cmd3 = w >> 8;
  xs->erase_commands[3].size = w >> 16;
  xs->erase_commands[3].cmd3 = w >> 24;

  w = xspi_read_sfdp(xs, ptpj + 0x24);
  for(int i = 0; i < 4; i++)
    xs->erase_commands[i].delay = calc_erase_time(w >> (4 + i * 7));

  if(ptp4) {
    w = xspi_read_sfdp(xs, ptp4 + 0x4);
    xs->erase_commands[0].cmd4 = w;
    xs->erase_commands[1].cmd4 = w >> 8;
    xs->erase_commands[2].cmd4 = w >> 16;
    xs->erase_commands[3].cmd4 = w >> 24;
  } else {
    for(int i = 0; i < 4; i++)
      xs->erase_commands[i].cmd4 = 0xff;
  }

  // Compute prescaler for ~50MHz target SPI clock
  unsigned int clk = clk_get_freq(CLK_XSPI2);
  xs->prescaler = clk / 50000000;
  if(xs->prescaler > 0)
    xs->prescaler--;

  // Write DCR1/DCR2 once — all operations reuse these
  reg_wr(xs->base + XSPI_DCR1,
         (xs->devsize << 16) | (0b010 << 24));
  reg_wr(xs->base + XSPI_DCR2, xs->prescaler);

  xs->iface.erase = xspi_erase;
  xs->iface.write = xspi_write;
  xs->iface.read = xspi_read;
  xs->iface.ctrl = xspi_ctrl;

  printf("%dMHz OK\n", clk / (xs->prescaler + 1) / 1000000);
  return &xs->iface;

 bad:
  free(xs);
  return NULL;
}
