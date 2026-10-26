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
#define XSPIM_BASE 0x5802b400  // XSPI I/O manager

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
  uint8_t bshift;    // log2 of block (erase unit) size
  uint8_t infineon;  // Infineon S25/S28: 4-byte opcodes, uniform 256 KB sectors

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

// Infineon S25/S28 (JEDEC manufacturer 0x34). Program/erase use the 4-byte
// address opcodes, errors are flagged in STR1, and the sectors must be
// uniform 256 KB: in the default hybrid layout the 4 KB erase is silently
// ignored outside the first 128 KB.
#define NOR_MFR_INFINEON 0x34
#define NOR_STR1_ERR     0x60  // PRGERR | ERSERR
#define NOR_CFR3V_ADDR   0x800004
#define NOR_CFR3N_ADDR   0x000004
#define NOR_CFR3_UNHYSA  0x08
#define NOR_CFR2_MEMLAT  8     // default read latency, non-volatile regs

static error_t xspi_cmd(xspi_t *xs, uint8_t cmd);

static error_t
xspi_wait_ready(xspi_t *xs)
{
  if(xs->state == XSPI_STATE_IDLE)
    return 0;

  if(xs->busy_until) {
    sleep_until(xs->busy_until);
    xs->busy_until = 0;
  }

  for(int i = 0; i < 5000; i++) {
    int status = xspi_get_status(xs);
    if((status & 1) == 0) {
      xs->state = XSPI_STATE_IDLE;
      if(xs->infineon && (status & NOR_STR1_ERR)) {
        xspi_cmd(xs, 0x82); // CLPEF: clear program/erase failure flags
        return ERR_FLASH_HW_ERROR;
      }
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


static uint32_t
xspi_read_id(xspi_t *xs)
{
  reg_wr(xs->base + XSPI_CR, (0b01 << 28) | 1); // FMODE = READ, Enable
  reg_wr(xs->base + XSPI_DCR1, (31 << 16) | (0b010 << 24));
  reg_wr(xs->base + XSPI_DCR2, 32);
  reg_wr(xs->base + XSPI_TCR, 0);
  reg_wr(xs->base + XSPI_DLR, 2); // 3 bytes
  reg_wr(xs->base + XSPI_CCR,
         (0b001 << 24) | // Data on single line
         (0b001 << 0));  // Instruction on single line
  reg_wr(xs->base + XSPI_IR, 0x9f);
  xspi_wait_tc(xs);
  return reg_rd(xs->base + XSPI_DR);
}

// Read Any Register, 3-byte address (CFR2V[7] = 0)
static uint8_t
xspi_rdarg(xspi_t *xs, uint32_t addr, uint32_t dummy)
{
  reg_wr(xs->base + XSPI_CR, (0b01 << 28) | 1); // FMODE = READ, Enable
  reg_wr(xs->base + XSPI_DLR, 0);
  reg_wr(xs->base + XSPI_TCR, dummy);
  reg_wr(xs->base + XSPI_CCR,
         (0b001 << 24) | // Data on single line
         (0b10 << 12) |  // 24-bit address
         (0b001 << 8) |  // Address on single line
         (0b001 << 0));  // Instruction on single line
  reg_wr(xs->base + XSPI_IR, 0x65);
  reg_wr(xs->base + XSPI_AR, addr);
  xspi_wait_tc(xs);
  uint8_t v = reg_rd8(xs->base + XSPI_DR);
  reg_wr(xs->base + XSPI_TCR, 0);
  return v;
}

static void
xspi_wrarg(xspi_t *xs, uint32_t addr, uint8_t val)
{
  xspi_cmd(xs, 6); // Write enable
  reg_wr(xs->base + XSPI_CR, (0b00 << 28) | 1); // FMODE = WRITE, Enable
  reg_wr(xs->base + XSPI_DLR, 0);
  reg_wr(xs->base + XSPI_CCR,
         (0b001 << 24) | (0b10 << 12) | (0b001 << 8) | (0b001 << 0));
  reg_wr(xs->base + XSPI_IR, 0x71);
  reg_wr(xs->base + XSPI_AR, addr);
  while(!(reg_rd(xs->base + XSPI_SR) & (1 << 2))) {} // FTF
  reg_wr8(xs->base + XSPI_DR, val);
  xspi_wait_tc(xs);
}

// Make sure an Infineon part uses uniform 256 KB sectors. UNHYSA is in
// CFR3N, and the volatile copy is read-only so a change needs a reset.
static error_t
xspi_infineon_uniform(xspi_t *xs)
{
  if(xspi_rdarg(xs, NOR_CFR3V_ADDR, 0) & NOR_CFR3_UNHYSA)
    return 0;

  printf("hybrid sectors, switching to uniform  ");

  uint8_t n = xspi_rdarg(xs, NOR_CFR3N_ADDR, NOR_CFR2_MEMLAT);
  if(!(n & NOR_CFR3_UNHYSA)) {
    xspi_wrarg(xs, NOR_CFR3N_ADDR, n | NOR_CFR3_UNHYSA);

    int status = 1;
    for(int i = 0; i < 1000 && (status & 1); i++) {
      status = xspi_get_status(xs);
      if(status & 1)
        usleep(1000);
    }
    if(status & 1)
      return ERR_FLASH_TIMEOUT;
    if(status & NOR_STR1_ERR) {
      xspi_cmd(xs, 0x82);
      return ERR_FLASH_HW_ERROR;
    }
  }

  xspi_cmd(xs, 0x66); // Software reset reloads CFR3V from CFR3N
  xspi_cmd(xs, 0x99);
  usleep(1000);

  if(!(xspi_rdarg(xs, NOR_CFR3V_ADDR, 0) & NOR_CFR3_UNHYSA))
    return ERR_FLASH_HW_ERROR;
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

    const uint32_t addr = block << xs->bshift;

    err = ERR_INVALID_ADDRESS;
    for(int i = 3; i >= 0; i--) {
      if(xs->erase_commands[i].size == 0)
        continue;

      if(xs->erase_commands[i].size < xs->bshift)
        continue;

      size_t chunk = 1 << (xs->erase_commands[i].size - xs->bshift);
      if(chunk > count)
        continue;
      if(addr & ((1 << xs->erase_commands[i].size) - 1))
        continue;

      // On Infineon the SFDP erase opcodes are the 4-byte address ones
      int use4byte = xs->infineon || addr > 0xffffff;
      uint8_t cmd = (use4byte && !xs->infineon) ? xs->erase_commands[i].cmd4
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

    error_t err = xspi_wait_ready(xs);
    if(err)
      return err;

    xspi_cmd(xs, 6); // Write enable

    uint32_t addr = xs->iface.block_size * block + offset;
    int use4byte = xs->infineon || addr > 0xffffff;

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

  // NOR flash is on XSPIM Port 2, reached via XSPI1 in swapped mode
  // (XSPIM_CR MODE=1: XSPI1 -> Port 2). Configure the mux here rather than
  // trusting the FSBL handoff — an older field FSBL may leave it in direct
  // mode (MODE=0, the reset default). Per RM0486 29.4.3 the XSPIM config can
  // only change while all XSPIs are disabled, so gate the controllers off
  // first. Writing XSPI1 DCR/CCR during SFDP probing relaunches the HS
  // interface calibration for the newly-selected XSPI (RM0486 29.5.4).
  clk_enable(CLK_XSPI1);
  clk_enable(CLK_XSPI2);
  clk_enable(CLK_XSPIM);
  reg_wr(XSPI1_BASE + XSPI_CR, 0);
  reg_wr(XSPI2_BASE + XSPI_CR, 0);
  reg_wr(XSPIM_BASE, 0x02); // MUXEN=0, MODE=1 (swapped): XSPI1 <-> Port 2

  gpio_conf_af(GPIO_PN(1), 9, GPIO_PUSH_PULL, GPIO_SPEED_HIGH, GPIO_PULL_NONE);
  gpio_conf_af(GPIO_PN(2), 9, GPIO_PUSH_PULL, GPIO_SPEED_HIGH, GPIO_PULL_NONE);
  gpio_conf_af(GPIO_PN(3), 9, GPIO_PUSH_PULL, GPIO_SPEED_HIGH, GPIO_PULL_NONE);
  gpio_conf_af(GPIO_PN(6), 9, GPIO_PUSH_PULL, GPIO_SPEED_HIGH, GPIO_PULL_NONE);

  xs->base = XSPI1_BASE;
  xs->devsize = 31; // Default for SFDP probing

  mutex_init(&xs->mutex, "xspi");
  task_waitable_init(&xs->waitq, "xspi");

  irq_enable_fn_arg(170, IRQ_LEVEL_SCHED, xspi_irq, xs); // XSPI1 (RM0486 t.135)

  printf("xspi%d: ", xs->base == XSPI1_BASE ? 1 : 2);

  // Probe the vendor: Infineon parts default to a hybrid sector layout
  // that the block layer cannot use. Everything else is driven from SFDP.
  if((xspi_read_id(xs) & 0xff) == NOR_MFR_INFINEON) {
    xs->infineon = 1;
    error_t err = xspi_infineon_uniform(xs);
    if(err) {
      printf("Unable to select uniform sectors: %s\n", error_to_string(err));
      goto bad;
    }
  }

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

  // Block size is the erase unit: 4 KB, or 256 KB on uniform Infineon parts
  xs->bshift = xs->infineon ? 18 : 12;
  xs->iface.num_blocks = size >> xs->bshift;
  xs->iface.block_size = 1 << xs->bshift;

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
  unsigned int clk = clk_get_freq(CLK_XSPI1);
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
