// Tegra T234 I2C Master driver
//
// Packet mode transfers using MST_FIFO registers, PIO (no DMA),
// interrupt-driven completion.

#include <stdlib.h>
#include <string.h>
#include <sys/uio.h>
#include <unistd.h>

#include <mios/io.h>
#include <mios/task.h>
#include <mios/mios.h>

#include "irq.h"
#include "reg.h"

#include "t234_i2c.h"
#include "t234ccplex_clk.h"

// ---------------------------------------------------------------
// Register offsets (TRM chapter 9.6)
// ---------------------------------------------------------------

#define I2C_CNFG                        0x000
#define I2C_SL_CNFG                     0x020
#define I2C_SL_ADDR1                    0x02c
#define I2C_SL_ADDR2                    0x030
#define I2C_TX_PACKET_FIFO              0x050
#define I2C_RX_FIFO                     0x054
#define I2C_INT_MASK                    0x064
#define I2C_INT_STATUS                  0x068
#define I2C_CLK_DIVISOR                 0x06c
#define I2C_CONFIG_LOAD                 0x08c
#define I2C_INTERFACE_TIMING_0          0x094
#define I2C_INTERFACE_TIMING_1          0x098
#define I2C_MASTER_RESET_CNTRL          0x0a8
#define I2C_MST_FIFO_CONTROL            0x0b4
#define I2C_MST_FIFO_STATUS             0x0b8

// I2C_CNFG bits
#define CNFG_PACKET_MODE_EN             (1 << 10)
#define CNFG_NEW_MASTER_FSM             (1 << 11)
#define CNFG_DEBOUNCE_CNT(x)            ((x) << 12)

// I2C_SL_CNFG bits
#define SL_CNFG_NACK                    (1 << 1)
#define SL_CNFG_NEWSL                   (1 << 2)

// Interrupt bits (shared between INT_MASK and INT_STATUS)
#define INT_ARB_LOST                    (1 << 2)
#define INT_NOACK                       (1 << 3)
#define INT_RFIFO_UNF                   (1 << 4)
#define INT_TFIFO_OVF                   (1 << 5)
#define INT_PACKET_XFER_COMPLETE        (1 << 7)

#define INT_ERR_MASK  (INT_NOACK | INT_ARB_LOST | INT_TFIFO_OVF | INT_RFIFO_UNF)
#define XFER_INT_MASK (INT_PACKET_XFER_COMPLETE | INT_ERR_MASK)

// MST_FIFO_CONTROL bits
#define MST_FIFO_CONTROL_RX_FLUSH       (1 << 0)
#define MST_FIFO_CONTROL_TX_FLUSH       (1 << 1)

// CONFIG_LOAD bits
#define CONFIG_LOAD_MSTR                (1 << 0)

// Protocol-Specific Header bits
#define I2C_HDR_REPEAT_START            (1 << 16)
#define I2C_HDR_IE                      (1 << 17)
#define I2C_HDR_READ                    (1 << 19)

// ---------------------------------------------------------------
// Clock / timing defaults for Fast Mode (400 kHz)
// Values from Linux tegra194 driver
// ---------------------------------------------------------------

#define CLK_DIVISOR_STD_FAST_MODE       0x3c
#define CLK_DIVISOR_HS_MODE             0x01
#define INTERFACE_TIMING_THIGH          0x02
#define INTERFACE_TIMING_TLOW           0x02
#define SETUP_HOLD_TIME                 0x02020202

// ---------------------------------------------------------------
// Per-instance configuration (from TRM / device tree)
// ---------------------------------------------------------------

struct t234_i2c_config {
  uint32_t base_addr;
  uint16_t spi_irq;   // GIC SPI number (actual IRQ = spi_irq + 32)
  uint16_t clk_id;    // BPMP clock ID
  uint16_t rst_id;    // BPMP reset ID
};

// bus_id is 1-indexed, table is 0-indexed
static const struct t234_i2c_config i2c_configs[] = {
  [0] = { 0x03160000, 25, 48, 24 },  // I2C1
  [1] = { 0x0c240000, 26, 49, 29 },  // I2C2
  [2] = { 0x03180000, 27, 50, 30 },  // I2C3
  [3] = { 0x03190000, 28, 51, 31 },  // I2C4
  [4] = { 0 },                        // I2C5 not present
  [5] = { 0x031b0000, 30, 52, 32 },  // I2C6
  [6] = { 0x031c0000, 31, 53, 33 },  // I2C7
  [7] = { 0x0c250000, 32, 54, 34 },  // I2C8
  [8] = { 0x031e0000, 33, 55, 35 },  // I2C9
};

// ---------------------------------------------------------------
// Driver struct
// ---------------------------------------------------------------

typedef struct t234_i2c {
  i2c_t i2c;
  uint32_t base_addr;
  task_waitable_t wait;
  error_t result;
  mutex_t mutex;
} t234_i2c_t;

#define T234_I2C_MAX_BUS 9

static t234_i2c_t *g_i2c[T234_I2C_MAX_BUS];

// ---------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------

static size_t
iov_total(const struct iovec *iov, size_t count)
{
  if(iov == NULL)
    return 0;

  size_t total = 0;
  for(size_t i = 0; i < count; i++)
    total += iov[i].iov_len;
  return total;
}


static void
flush_fifos(t234_i2c_t *i2c)
{
  uint32_t base = i2c->base_addr;

  reg_wr(base + I2C_MST_FIFO_CONTROL,
         MST_FIFO_CONTROL_TX_FLUSH | MST_FIFO_CONTROL_RX_FLUSH);

  // Flush bits are auto-cleared by hardware
  for(int i = 0; i < 1000; i++) {
    if(!(reg_rd(base + I2C_MST_FIFO_CONTROL) &
         (MST_FIFO_CONTROL_TX_FLUSH | MST_FIFO_CONTROL_RX_FLUSH)))
      return;
  }
}


static void
push_packet_header(t234_i2c_t *i2c, uint8_t addr, size_t payload_len,
                   int is_read, int repeat_start)
{
  uint32_t base = i2c->base_addr;

  // Header Word 0: Protocol=I2C(1), PktId=1
  reg_wr(base + I2C_TX_PACKET_FIFO, (1 << 4) | (1 << 16));

  // Header Word 1: PayloadSize = bytes - 1
  reg_wr(base + I2C_TX_PACKET_FIFO, payload_len - 1);

  // Protocol-Specific Header
  uint32_t hdr = ((uint32_t)addr << 1) | I2C_HDR_IE;
  if(is_read)
    hdr |= I2C_HDR_READ;
  if(repeat_start)
    hdr |= I2C_HDR_REPEAT_START;

  reg_wr(base + I2C_TX_PACKET_FIFO, hdr);
}

// ---------------------------------------------------------------
// ISR
// ---------------------------------------------------------------

static void
t234_i2c_irq(void *arg)
{
  t234_i2c_t *i2c = arg;
  uint32_t base = i2c->base_addr;

  uint32_t status = reg_rd(base + I2C_INT_STATUS);
  reg_wr(base + I2C_INT_STATUS, status);

  if(i2c->result != 1)
    return;

  if(status & INT_NOACK) {
    reg_wr(base + I2C_INT_MASK, 0);
    i2c->result = ERR_NO_DEVICE;
    task_wakeup(&i2c->wait, 0);
    return;
  }

  if(status & INT_ARB_LOST) {
    reg_wr(base + I2C_INT_MASK, 0);
    i2c->result = ERR_ARBITRATION_LOST;
    task_wakeup(&i2c->wait, 0);
    return;
  }

  if(status & (INT_TFIFO_OVF | INT_RFIFO_UNF)) {
    reg_wr(base + I2C_INT_MASK, 0);
    i2c->result = ERR_BUS_ERROR;
    task_wakeup(&i2c->wait, 0);
    return;
  }

  if(status & INT_PACKET_XFER_COMPLETE) {
    reg_wr(base + I2C_INT_MASK, 0);
    i2c->result = ERR_OK;
    task_wakeup(&i2c->wait, 0);
    return;
  }
}

// ---------------------------------------------------------------
// Transfer
// ---------------------------------------------------------------

static error_t
xfer_write(t234_i2c_t *i2c, uint8_t addr,
           const struct iovec *iov, size_t count,
           size_t total_len, int repeat_start,
           int64_t deadline)
{
  uint32_t base = i2c->base_addr;

  flush_fifos(i2c);
  reg_wr(base + I2C_INT_STATUS, reg_rd(base + I2C_INT_STATUS));

  i2c->result = 1;

  push_packet_header(i2c, addr, total_len, 0, repeat_start);

  // Push write data into TX FIFO (max 128 words including header)
  uint32_t word = 0;
  int byte_pos = 0;

  for(size_t i = 0; i < count; i++) {
    const uint8_t *data = iov[i].iov_base;
    for(size_t j = 0; j < iov[i].iov_len; j++) {
      word |= (uint32_t)(data ? data[j] : 0) << (byte_pos * 8);
      byte_pos++;
      if(byte_pos == 4) {
        reg_wr(base + I2C_TX_PACKET_FIFO, word);
        word = 0;
        byte_pos = 0;
      }
    }
  }
  if(byte_pos > 0)
    reg_wr(base + I2C_TX_PACKET_FIFO, word);

  reg_wr(base + I2C_INT_MASK, XFER_INT_MASK);

  error_t r;
  while((r = i2c->result) == 1) {
    if(task_sleep_deadline(&i2c->wait, deadline)) {
      i2c->result = 0;
      reg_wr(base + I2C_INT_MASK, 0);
      return ERR_TIMEOUT;
    }
  }

  return r;
}


static error_t
xfer_read(t234_i2c_t *i2c, uint8_t addr,
          const struct iovec *iov, size_t count,
          size_t total_len, int64_t deadline)
{
  uint32_t base = i2c->base_addr;

  flush_fifos(i2c);
  reg_wr(base + I2C_INT_STATUS, reg_rd(base + I2C_INT_STATUS));

  i2c->result = 1;

  push_packet_header(i2c, addr, total_len, 1, 0);

  reg_wr(base + I2C_INT_MASK, XFER_INT_MASK);

  error_t r;
  while((r = i2c->result) == 1) {
    if(task_sleep_deadline(&i2c->wait, deadline)) {
      i2c->result = 0;
      reg_wr(base + I2C_INT_MASK, 0);
      return ERR_TIMEOUT;
    }
  }

  if(r != ERR_OK)
    return r;

  // Drain RX FIFO
  uint32_t word = 0;
  int byte_pos = 4;  // Force initial FIFO read

  for(size_t i = 0; i < count; i++) {
    uint8_t *data = iov[i].iov_base;
    for(size_t j = 0; j < iov[i].iov_len; j++) {
      if(byte_pos >= 4) {
        word = reg_rd(base + I2C_RX_FIFO);
        byte_pos = 0;
      }
      data[j] = (word >> (byte_pos * 8)) & 0xff;
      byte_pos++;
    }
  }

  return ERR_OK;
}

// ---------------------------------------------------------------
// Public interface
// ---------------------------------------------------------------

static error_t
t234_i2c_rwv(i2c_t *d, uint8_t addr, const struct iovec *txiov,
             const struct iovec *rxiov, size_t count)
{
  t234_i2c_t *i2c = (t234_i2c_t *)d;

  const size_t write_len = iov_total(txiov, count);
  const size_t read_len = iov_total(rxiov, count);

  if(write_len == 0 && read_len == 0)
    return ERR_OK;

  mutex_lock(&i2c->mutex);

  const int64_t deadline = clock_get() + 100000;
  const int q = irq_forbid(IRQ_LEVEL_IO);

  error_t r = ERR_OK;

  if(write_len > 0)
    r = xfer_write(i2c, addr, txiov, count, write_len,
                   read_len > 0, deadline);

  if(r == ERR_OK && read_len > 0)
    r = xfer_read(i2c, addr, rxiov, count, read_len, deadline);

  irq_permit(q);
  mutex_unlock(&i2c->mutex);
  return r;
}


static error_t
init_hw(t234_i2c_t *i2c)
{
  uint32_t base = i2c->base_addr;

  // Soft reset master logic
  reg_wr(base + I2C_MASTER_RESET_CNTRL, 1);
  udelay(2);
  reg_wr(base + I2C_MASTER_RESET_CNTRL, 0);
  udelay(2);

  // Packet mode, new master FSM, debounce count = 2
  reg_wr(base + I2C_CNFG,
         CNFG_PACKET_MODE_EN | CNFG_NEW_MASTER_FSM | CNFG_DEBOUNCE_CNT(2));

  // Disable all interrupts
  reg_wr(base + I2C_INT_MASK, 0);

  // Clock divisor (fast mode)
  reg_wr(base + I2C_CLK_DIVISOR,
         (CLK_DIVISOR_STD_FAST_MODE << 16) | CLK_DIVISOR_HS_MODE);

  // Interface timing (fast mode)
  reg_wr(base + I2C_INTERFACE_TIMING_0,
         (INTERFACE_TIMING_THIGH << 8) | INTERFACE_TIMING_TLOW);
  reg_wr(base + I2C_INTERFACE_TIMING_1, SETUP_HOLD_TIME);

  // Configure slave to NACK (we are master-only)
  reg_wr(base + I2C_SL_CNFG,
         reg_rd(base + I2C_SL_CNFG) | SL_CNFG_NACK | SL_CNFG_NEWSL);
  reg_wr(base + I2C_SL_ADDR1, 0xfc);
  reg_wr(base + I2C_SL_ADDR2, 0x00);

  flush_fifos(i2c);

  // Load master configuration
  reg_wr(base + I2C_CONFIG_LOAD, CONFIG_LOAD_MSTR);
  for(int i = 0; i < 10000; i++) {
    if(!(reg_rd(base + I2C_CONFIG_LOAD) & CONFIG_LOAD_MSTR))
      return ERR_OK;
    udelay(1);
  }

  return ERR_TIMEOUT;
}


i2c_t *
t234_i2c_create(unsigned int bus_id)
{
  if(bus_id < 1 || bus_id > T234_I2C_MAX_BUS)
    return NULL;

  unsigned int idx = bus_id - 1;
  const struct t234_i2c_config *cfg = &i2c_configs[idx];

  if(cfg->base_addr == 0)
    return NULL;

  if(g_i2c[idx])
    return NULL;

  // Enable clock and deassert reset via BPMP
  clk_enable(cfg->clk_id);
  reset_peripheral(cfg->rst_id);

  t234_i2c_t *i2c = calloc(1, sizeof(t234_i2c_t));
  i2c->base_addr = cfg->base_addr;
  i2c->i2c.rwv = t234_i2c_rwv;

  task_waitable_init(&i2c->wait, "i2c");
  mutex_init(&i2c->mutex, "i2c");

  if(init_hw(i2c)) {
    free(i2c);
    return NULL;
  }

  irq_enable_fn_arg(cfg->spi_irq + 32, IRQ_LEVEL_IO, t234_i2c_irq, i2c);

  g_i2c[idx] = i2c;
  return &i2c->i2c;
}


i2c_t *
i2c_get_bus(unsigned int bus_id)
{
  bus_id--;
  if(bus_id >= T234_I2C_MAX_BUS)
    return NULL;
  return (i2c_t *)g_i2c[bus_id];
}
