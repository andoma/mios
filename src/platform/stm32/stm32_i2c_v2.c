// This file is not compiled on its own but needs to be included
// by a stm32 chip specific file


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

  const uint8_t *write;
  size_t write_len;
  uint8_t *read;
  size_t read_len;

  mutex_t mutex;

} stm32_i2c_t;


static void
i2c_done(stm32_i2c_t *i2c, error_t result)
{
  i2c->result = result;
  reg_wr(i2c->base_addr + I2C_CR1, 0);
  task_wakeup(&i2c->wait, 0);
}


static void
i2c_irq(stm32_i2c_t *i2c)
{
  uint32_t isr = reg_rd(i2c->base_addr + I2C_ISR);

  if(isr & 0x2 && i2c->write_len) {
    // Output next TX byte
    reg_wr(i2c->base_addr + I2C_TXDR, *i2c->write);
    i2c->write++;
    i2c->write_len--;
    reg_wr(i2c->base_addr + I2C_ICR, 2);
    return;
  }

  if(isr & 0x4 && i2c->read_len) {
    // New RX byte available
    *i2c->read = reg_rd(i2c->base_addr + I2C_RXDR);
    i2c->read++;
    i2c->read_len--;
    reg_wr(i2c->base_addr + I2C_ICR, 4);
    return;
  }

  if(i2c->result != 1) {
    // Thread is no longer waiting, cancel everything
    reg_wr(i2c->base_addr + I2C_ICR, isr);
    return;
  }

  if(isr & 0x20 && !i2c->write_len && !i2c->read_len) {
    // Completed
    return i2c_done(i2c, ERR_OK);
  }

  if(isr & 0x10) {
    // NACK
    return i2c_done(i2c, i2c->write_len ? ERR_TX : ERR_RX);
  }

  if(isr & 0x100)
    return i2c_done(i2c, ERR_BUS_ERROR);
  if(isr & 0x200)
    return i2c_done(i2c, ERR_ARBITRATION_LOST);

  if(!i2c->write_len && i2c->read_len) {
    // Write cycle completed, initiate read

    reg_wr(i2c->base_addr + I2C_CR2,
           (reg_rd(i2c->base_addr + I2C_CR2) & 0x3ff) |
           (1 << 10) | // READ
           (1 << 13) | // Generate Start
           (1 << 25) | // AutoEnd
           (i2c->read_len << 16));
    return;
  }
}





static error_t
i2c_rw(i2c_t *d, uint8_t addr, const uint8_t *write, size_t write_len,
       uint8_t *read, size_t read_len)
{
  stm32_i2c_t *i2c = (stm32_i2c_t *)d;

  mutex_lock(&i2c->mutex);

  i2c->write = write;
  i2c->write_len = write_len;
  i2c->read = read;
  i2c->read_len = read_len;

  const int64_t deadline = clock_get() + 100000;

  const int q = irq_forbid(IRQ_LEVEL_IO);

  i2c->result = 1; // 1 means 'no result yet'

  reg_wr(i2c->base_addr + I2C_CR1, 0xff);

  if(write_len) {
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

  error_t r;

  while((r = i2c->result) == 1) {
    if(task_sleep_deadline(&i2c->wait, deadline)) {
      i2c->result = 0;
      i2c->read_len = 0;
      i2c->write_len = 0;
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
  d->i2c.rw = i2c_rw;
  reg_wr(d->base_addr + I2C_CR1, 0);
  task_waitable_init(&d->wait, "i2c");
  mutex_init(&d->mutex, "i2clock");
  reg_wr(d->base_addr + I2C_CR1, 0);
  return d;
}
