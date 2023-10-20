// This file is not compiled on its own but needs to be included
// by a stm32 chip specific file

#include <sys/uio.h>

#define I2C_CR1       0x00
#define I2C_CR2       0x04
#define I2C_TIMINGR   0x10
#define I2C_TIMEOUTR  0x14
#define I2C_ISR       0x18
#define I2C_ICR       0x1c
#define I2C_RXDR      0x24
#define I2C_TXDR      0x28


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

  mutex_t mutex;

} stm32_i2c_t;


static void
i2c_init_off(stm32_i2c_t *i2c)
{
  i2c->iov_off = 0;
  i2c->data_off = 0;
}

static void
i2c_done(stm32_i2c_t *i2c, error_t result)
{
  i2c->result = result;
  reg_wr(i2c->base_addr + I2C_CR1, 0);
  task_wakeup(&i2c->wait, 0);
}


static size_t
iov_size(const struct iovec *iov, size_t count)
{
  if(iov == NULL)
    return 0;

  size_t total = 0;
  for(size_t i = 0; i < count; i++) {
    total += iov[i].iov_len;
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
i2c_irq(void *arg)
{
  stm32_i2c_t *i2c = arg;

  uint32_t isr = reg_rd(i2c->base_addr + I2C_ISR);

  if(isr & 0x2 && i2c->txiov) {
    // Output next TX byte
    const uint8_t *base = i2c->txiov[i2c->iov_off].iov_base;
    reg_wr(i2c->base_addr + I2C_TXDR, base ? base[i2c->data_off] : 0);
    i2c->txiov = iov_inc(i2c, i2c->txiov);
    reg_wr(i2c->base_addr + I2C_ICR, 2);
    return;
  }

  if(isr & 0x4 && i2c->rxiov) {
    // New RX byte available
    uint8_t *base = i2c->rxiov[i2c->iov_off].iov_base;
    base[i2c->data_off] = reg_rd(i2c->base_addr + I2C_RXDR);
    i2c->rxiov = iov_inc(i2c, i2c->rxiov);
    reg_wr(i2c->base_addr + I2C_ICR, 4);
    return;
  }

  if(i2c->result != 1) {
    // Thread is no longer waiting, cancel everything
    reg_wr(i2c->base_addr + I2C_ICR, isr);
    return;
  }

  if(isr & 0x20 && !i2c->txiov && !i2c->rxiov) {
    // Completed
    return i2c_done(i2c, ERR_OK);
  }

  if(isr & 0x10) {
    // NACK
    return i2c_done(i2c, i2c->txiov ? ERR_TX : ERR_RX);
  }

  if(isr & 0x100)
    return i2c_done(i2c, ERR_BUS_ERROR);
  if(isr & 0x200)
    return i2c_done(i2c, ERR_ARBITRATION_LOST);

  if(!i2c->txiov && i2c->rxiov) {
    // Write cycle completed, initiate read

    i2c_init_off(i2c);

    const size_t read_len = iov_size(i2c->rxiov, i2c->count);

    reg_wr(i2c->base_addr + I2C_CR2,
           (reg_rd(i2c->base_addr + I2C_CR2) & 0x3ff) |
           (1 << 10) | // READ
           (1 << 13) | // Generate Start
           (1 << 25) | // AutoEnd
           (read_len << 16));
    return;
  }
}




static error_t
i2c_rwv(i2c_t *d, uint8_t addr, const struct iovec *txiov,
        const struct iovec *rxiov, size_t count)
{
  stm32_i2c_t *i2c = (stm32_i2c_t *)d;

  const size_t read_len = iov_size(rxiov, count);
  const size_t write_len = iov_size(txiov, count);

  if(read_len > 255 || write_len > 255)
    return ERR_MTU_EXCEEDED;

  mutex_lock(&i2c->mutex);

  i2c->count = count;
  i2c->txiov = txiov;
  i2c->rxiov = rxiov;

  const int64_t deadline = clock_get() + 100000;

  const int q = irq_forbid(IRQ_LEVEL_IO);

  i2c->result = 1; // 1 means 'no result yet'

  reg_wr(i2c->base_addr + I2C_CR1, 0xff);

  if(txiov) {

    reg_wr(i2c->base_addr + I2C_CR2,
           (addr << 1) |
           (1 << 13) | // Generate Start
           (!read_len ? (1 << 25) : 0) | // AutoEnd
           (write_len << 16));
  } else {

    reg_wr(i2c->base_addr + I2C_CR2,
           (addr << 1) |
           (1 << 10) | // READ
           (1 << 13) | // Generate Start
           (1 << 25) | // AutoEnd
           (read_len << 16));
  }

  i2c_init_off(i2c);

  error_t r;

  while((r = i2c->result) == 1) {
    if(task_sleep_deadline(&i2c->wait, deadline)) {
      i2c->result = 0;
      irq_permit(q);
      mutex_unlock(&i2c->mutex);
      return ERR_TIMEOUT;
    }
  }

  irq_permit(q);
  mutex_unlock(&i2c->mutex);
  return r;
}


static stm32_i2c_t *
stm32_i2c_create(uint32_t base_addr)
{
  stm32_i2c_t *d = malloc(sizeof(stm32_i2c_t));
  d->base_addr = base_addr;
  d->i2c.rwv = i2c_rwv;
  reg_wr(d->base_addr + I2C_CR1, 0);
  task_waitable_init(&d->wait, "i2c");
  mutex_init(&d->mutex, "i2clock");
  reg_wr(d->base_addr + I2C_CR1, 0);
  return d;
}
