// This file is not compiled on its own but needs to be included
// by a stm32 chip specific file

#include <stdio.h>
#include <stdlib.h>
#include <mios/task.h>
#include <unistd.h>
#include <assert.h>
#include <sys/uio.h>

#define I2C_LOG_SIZE 0

#define I2C_CR1   0x00
#define I2C_CR2   0x04
#define I2C_OAR1  0x08
#define I2C_OAR2  0x0C
#define I2C_DR    0x10
#define I2C_SR1   0x14
#define I2C_SR2   0x18
#define I2C_CCR   0x1c
#define I2C_TRISE 0x20
#define I2C_FLTR  0x24

#define I2C_CR1_START_BIT 8
#define I2C_CR1_STOP_BIT  9

typedef struct stm32_i2c {
  struct i2c i2c;
  uint32_t base_addr;
  task_waitable_t wait;

  error_t result;
  const struct iovec *txiov;
  const struct iovec *rxiov;
  size_t count;
  size_t iov_off;
  size_t data_off;
  uint8_t txdone;

  mutex_t mutex;

  uint32_t freq_mhz;

  uint16_t clkid;
  uint16_t rstid;
  uint8_t addr;

} stm32_i2c_t;


#if I2C_LOG_SIZE > 0

#include <mios/cli.h>

typedef struct {
  int64_t ts;
  uint16_t event;
  uint16_t value;
} i2c_log_t;

#define I2C_LOG_EVENT_EV      1
#define I2C_LOG_EVENT_ER      2
#define I2C_LOG_EVENT_WC      3
#define I2C_LOG_EVENT_RC      4
#define I2C_LOG_EVENT_START   5
#define I2C_LOG_EVENT_DONE    6
#define I2C_LOG_EVENT_SB      7
#define I2C_LOG_EVENT_WR      8
#define I2C_LOG_EVENT_AC      9


static i2c_log_t i2c_log[I2C_LOG_SIZE];
static int l2c_log_ptr;


static void
i2c_log_append(uint16_t event, uint16_t value)
{
  size_t ptr = l2c_log_ptr & (I2C_LOG_SIZE - 1);
  i2c_log[ptr].ts = clock_get_irq_blocked();
  i2c_log[ptr].event = event;
  i2c_log[ptr].value = value;
  l2c_log_ptr++;
}


static void
print_i2clog(stream_t *st)
{
  int64_t t0 = 0;
  for(size_t i = 0; i < I2C_LOG_SIZE; i++) {
    size_t ptr = (i + l2c_log_ptr) & (I2C_LOG_SIZE - 1);

    i2c_log_t *il = &i2c_log[ptr];

    if(il->event == 0)
      continue;
    if(t0 == 0)
      t0 = il->ts;

    int td = il->ts - t0;
    stprintf(st, "%10d : ", td);
    switch(il->event) {
    case I2C_LOG_EVENT_EV:
      stprintf(st, "IRQ EVT 0x%x\n", il->value);
      break;
    case I2C_LOG_EVENT_ER:
      stprintf(st, "IRQ ERR 0x%x\n", il->value);
      break;
    case I2C_LOG_EVENT_WC:
      stprintf(st, "WR COMP 0x%x\n", il->value);
      break;
    case I2C_LOG_EVENT_RC:
      stprintf(st, "RD COMP 0x%x\n", il->value);
      break;
    case I2C_LOG_EVENT_START:
      stprintf(st, "Start addr: 0x%x\n", il->value);
      break;
    case I2C_LOG_EVENT_DONE:
      stprintf(st, "Done  addr: 0x%x\n", il->value);
      break;
    case I2C_LOG_EVENT_SB:
      stprintf(st, "STARTBIT  0x%x mode=%s\n",
               il->value >> 1, il->value & 1 ? "READ" : "WRITE");
      break;
    case I2C_LOG_EVENT_WR:
      stprintf(st, "XMIT BYTE: 0x%x\n", il->value);
      break;
    case I2C_LOG_EVENT_AC:
      stprintf(st, "ADDR COMPLETE\n");
      break;
    }
  }
}


static error_t
cmd_i2clog(cli_t *cli, int argc, char **argv)
{
  print_i2clog(cli->cl_stream);
  return 0;
}

CLI_CMD_DEF("i2clog", cmd_i2clog);


#else

#define i2c_log_append(e, v)

#endif


static void
i2c_init_off(stm32_i2c_t *i2c)
{
  i2c->iov_off = 0;
  i2c->data_off = 0;
}

static size_t
iov_len(stm32_i2c_t *i2c, const struct iovec *iov)
{
  if(iov == NULL)
    return 0;

  size_t total = 0;
  int first = 1;
  for(size_t i = i2c->iov_off; i < i2c->count; i++) {
    total += iov[i].iov_len - (first ? i2c->data_off : 0);
    first = 0;
  }
  return total;
}


const struct iovec *
iov_inc(stm32_i2c_t *i2c, const struct iovec *iov)
{
  i2c->data_off++;
  if(i2c->data_off == iov[i2c->iov_off].iov_len) {
    i2c->iov_off++;
    i2c->data_off = 0;
    if(i2c->iov_off == i2c->count) {
      return NULL;
    }
  }
  return iov;
}


static void
i2c_irq_result(stm32_i2c_t *i2c, error_t result)
{
  i2c->result = result;
  task_wakeup(&i2c->wait, 0);
}

static void
set_read_ack(stm32_i2c_t *i2c)
{
  if(iov_len(i2c, i2c->rxiov) > 1) {
    reg_set_bit(i2c->base_addr + I2C_CR1, 10);
  } else {
    reg_clr_bit(i2c->base_addr + I2C_CR1, 10);
  }
}



static void
i2c_irq_ev(stm32_i2c_t *i2c)
{
  uint32_t sr1 = reg_rd(i2c->base_addr + I2C_SR1);

  i2c_log_append(I2C_LOG_EVENT_EV, sr1);
  size_t read_len;
  if(sr1 & (1 << 0)) {
    sr1 &= ~(1 << 0);
    // SB
    size_t write_len = iov_len(i2c, i2c->txiov);
    const int read_bit = write_len == 0;

    uint8_t byte = (i2c->addr << 1) | read_bit;
    i2c_log_append(I2C_LOG_EVENT_SB, byte);

    reg_wr(i2c->base_addr + I2C_DR, byte);
    return;
  }
  if(sr1 & (1 << 1)) {
    sr1 &= ~(1 << 1);
    size_t write_len = iov_len(i2c, i2c->txiov);

    // ADDR complete

    i2c_log_append(I2C_LOG_EVENT_AC, 0);

    reg_rd(i2c->base_addr + I2C_SR2);

    if(write_len == 0) {
      read_len = iov_len(i2c, i2c->rxiov);
      i2c_log_append(I2C_LOG_EVENT_WC, read_len);
      set_read_ack(i2c);
      reg_set_bit(i2c->base_addr + I2C_CR2, 10);
    }
  }
  if(sr1 & (1 << 6) && (read_len = iov_len(i2c, i2c->rxiov))) {
    sr1 &= ~(1 << 6);
    uint8_t *base = i2c->rxiov[i2c->iov_off].iov_base;
    base[i2c->data_off] = reg_rd(i2c->base_addr + I2C_DR);
    i2c->rxiov = iov_inc(i2c, i2c->rxiov);
    read_len--;

    if(read_len == 0) {
      i2c_log_append(I2C_LOG_EVENT_RC, 0);
      reg_set_bit(i2c->base_addr + I2C_CR1, I2C_CR1_STOP_BIT);
      reg_clr_bit(i2c->base_addr + I2C_CR2, 10);
      i2c_irq_result(i2c, 0);
    } else {
      set_read_ack(i2c);
    }
    return;
  }

  if(sr1 & (1 << 7) && !i2c->txdone) {
    sr1 &= ~(1 << 7);
    // TxE
    if(!i2c->txiov) {
      // need to re-read read_len
      i2c_init_off(i2c);
      read_len = iov_len(i2c, i2c->rxiov);
      i2c_log_append(I2C_LOG_EVENT_WC, read_len);

      if(sr1 & 0x4) {
        reg_rd(i2c->base_addr + I2C_DR);
      }

      if(read_len) {
        reg_set_bit(i2c->base_addr + I2C_CR1, I2C_CR1_START_BIT);
      } else {
        reg_set_bit(i2c->base_addr + I2C_CR1, I2C_CR1_STOP_BIT);
        i2c_irq_result(i2c, 0);
      }
      i2c->txdone = 1;
    } else {
      size_t write_len = iov_len(i2c, i2c->txiov);
      write_len--;
      if(write_len == 0) {
        reg_clr_bit(i2c->base_addr + I2C_CR2, 10);
      } else {
        reg_set_bit(i2c->base_addr + I2C_CR2, 10);
      }
      const uint8_t *base = i2c->txiov[i2c->iov_off].iov_base;
      i2c_log_append(I2C_LOG_EVENT_WR, base ? base[i2c->data_off] : 0);
      reg_wr(i2c->base_addr + I2C_DR, base ? base[i2c->data_off] : 0);
      i2c->txiov = iov_inc(i2c, i2c->txiov);
    }
  }
}


static void
i2c_irq_er(stm32_i2c_t *i2c)
{
  const uint32_t sr1 = reg_rd(i2c->base_addr + I2C_SR1);
  if(sr1 & (1 << 10)) {
    // Acknowledge failure
    reg_set_bit(i2c->base_addr + I2C_CR1, I2C_CR1_STOP_BIT);
    reg_wr(i2c->base_addr + I2C_SR1, sr1 & ~(1 << 10));
    return i2c_irq_result(i2c, ERR_NO_DEVICE);
  }
  if(sr1 & (1 << 8)) {
    reg_wr(i2c->base_addr + I2C_SR1, sr1 & ~(1 << 8));
    return i2c_irq_result(i2c, ERR_BUS_ERROR);
  }
  panic("%s: sr1=%x\n", __FUNCTION__, sr1);
}

static void
i2c_initialize(stm32_i2c_t *i2c)
{
  reset_peripheral(i2c->rstid);
  udelay(100);
  reg_wr(i2c->base_addr + I2C_CR1, 0);
  udelay(1000);
  reg_wr(i2c->base_addr + I2C_CR1, 1);
  udelay(1000);
  if(reg_rd(i2c->base_addr + I2C_SR2) & 2) {
    reg_wr(i2c->base_addr + I2C_CR1, 0x8001);
    while(reg_rd(i2c->base_addr + I2C_SR2) & 2) {
    }
    reg_wr(i2c->base_addr + I2C_CR1, 0x0);
    udelay(100);
    reg_wr(i2c->base_addr + I2C_CR1, 0x1);
    udelay(100);
  }

  reg_wr(i2c->base_addr + I2C_OAR1, (1 << 14)); // Must be 1 according to DS

  reg_wr(i2c->base_addr + I2C_CCR, i2c->freq_mhz * 5);

  reg_wr(i2c->base_addr + I2C_CR2,
         (1 << 9) |
         (1 << 8) |
         i2c->freq_mhz);

  reg_wr(i2c->base_addr + I2C_TRISE, i2c->freq_mhz + 1);
}



static error_t
i2c_rwv(struct i2c *d, uint8_t addr,
        const struct iovec *txiov,
        const struct iovec *rxiov,
        size_t count)
{
  stm32_i2c_t *i2c = (stm32_i2c_t *)d;

  mutex_lock(&i2c->mutex);

  i2c->addr = addr;
  i2c->txiov = txiov;
  i2c->rxiov = rxiov;
  i2c->count = count;
  i2c->txdone = txiov ? 0 : 1;

  const int64_t deadline = clock_get() + 100000;

  const int q = irq_forbid(IRQ_LEVEL_IO);

  i2c_log_append(I2C_LOG_EVENT_START, addr);

  i2c->result = 1; // 1 means 'no result yet'

  reg_set_bit(i2c->base_addr + I2C_CR1, I2C_CR1_START_BIT);

  i2c_init_off(i2c);

  error_t err;
  while((err = i2c->result) == 1) {
    if(task_sleep_deadline(&i2c->wait, deadline)) {
      i2c->result = 0;
      i2c_initialize(i2c);
      err = ERR_TIMEOUT;
      break;
    }
  }

  reg_clr_bit(i2c->base_addr + I2C_CR2, 10); // Disable xfer IRQ
  i2c_log_append(I2C_LOG_EVENT_DONE, addr);
  irq_permit(q);
  mutex_unlock(&i2c->mutex);
  return err;
}


static stm32_i2c_t *
stm32_i2c_create(uint32_t base_addr, uint16_t clkid, uint16_t rstid)
{
  stm32_i2c_t *i2c = malloc(sizeof(stm32_i2c_t));
  i2c->clkid = clkid;
  i2c->rstid = rstid;
  i2c->base_addr = base_addr;
  i2c->freq_mhz = clk_get_freq(clkid) / 1000000;
  i2c->i2c.rwv = i2c_rwv;

  i2c_initialize(i2c);
  task_waitable_init(&i2c->wait, "i2c");
  mutex_init(&i2c->mutex, "i2cmtx");
  return i2c;
}
