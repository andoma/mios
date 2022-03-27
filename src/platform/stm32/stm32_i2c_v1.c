// This file is not compiled on its own but needs to be included
// by a stm32 chip specific file

#include <stdio.h>
#include <stdlib.h>
#include <mios/task.h>
#include <unistd.h>

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
  const uint8_t *write;
  size_t write_len;
  uint8_t *read;
  size_t read_len;

  mutex_t mutex;

  uint8_t addr;

} stm32_i2c_t;



static void
i2c_irq_result(stm32_i2c_t *i2c, error_t result)
{
  i2c->result = result;
  task_wakeup(&i2c->wait, 0);
}

static void
set_read_ack(stm32_i2c_t *i2c)
{
  if(i2c->read_len > 1) {
    reg_set_bit(i2c->base_addr + I2C_CR1, 10);
  } else {
    reg_clr_bit(i2c->base_addr + I2C_CR1, 10);
  }
}


static void
i2c_irq_ev(stm32_i2c_t *i2c)
{
  uint32_t sr1 = reg_rd(i2c->base_addr + I2C_SR1);
#if 0
  printf("i2c: %d/%d %x\n",
         i2c->write_len, i2c->read_len, sr1);
#endif
  if(sr1 & (1 << 0)) {
    sr1 &= ~(1 << 0);
    // SB
    const int read_bit = i2c->write_len == 0;
    reg_wr(i2c->base_addr + I2C_DR, (i2c->addr << 1) | read_bit);
    return;
  }

  if(sr1 & (1 << 1)) {
    // ADDR complete
    reg_rd(i2c->base_addr + I2C_SR2);

    sr1 &= ~(1 << 1);
    if(i2c->write_len == 0) {
      set_read_ack(i2c);
      reg_set_bit(i2c->base_addr + I2C_CR2, 10);
    }

  }

  if(sr1 & (1 << 6) && i2c->read_len) {
    sr1 &= ~(1 << 6);

    *i2c->read = reg_rd(i2c->base_addr + I2C_DR);
    //    printf("RxNE: %x\n", *i2c->read);
    i2c->read++;
    i2c->read_len--;

    if(i2c->read_len == 0) {
      reg_set_bit(i2c->base_addr + I2C_CR1, I2C_CR1_STOP_BIT);
      reg_clr_bit(i2c->base_addr + I2C_CR2, 10);
      i2c_irq_result(i2c, 0);
    } else {
      set_read_ack(i2c);
    }
    return;
  }

  if(sr1 & (1 << 7) && i2c->write) {
    sr1 &= ~(1 << 7);
    // TxE
    //    printf("TxE %d\n", i2c->write_len);

    if(i2c->write_len == 0) {
      i2c->write = NULL;
      reg_clr_bit(i2c->base_addr + I2C_CR2, 10);
      if(i2c->read_len) {
        reg_set_bit(i2c->base_addr + I2C_CR1, I2C_CR1_START_BIT);
      } else {
        reg_set_bit(i2c->base_addr + I2C_CR1, I2C_CR1_STOP_BIT);
        i2c_irq_result(i2c, 0);
        //        printf("TXE done\n");
      }
    } else {

      reg_set_bit(i2c->base_addr + I2C_CR2, 10);
      reg_wr(i2c->base_addr + I2C_DR, *i2c->write);
      i2c->write++;
      i2c->write_len--;
    }
    return;
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
  printf("%s: sr1=%x\n", __FUNCTION__, sr1);
}


static error_t
i2c_rw(i2c_t *d, uint8_t addr, const uint8_t *write, size_t write_len,
       uint8_t *read, size_t read_len)
{
  stm32_i2c_t *i2c = (stm32_i2c_t *)d;

  //  hexdump("WRITE", write, write_len);

  mutex_lock(&i2c->mutex);

  i2c->addr = addr;
  i2c->write = write_len ? write : NULL;
  i2c->write_len = write_len;
  i2c->read = read;
  i2c->read_len = read_len;

  const int64_t deadline = clock_get() + 1000000;

  const int q = irq_forbid(IRQ_LEVEL_IO);

  i2c->result = 1; // 1 means 'no result yet'


  reg_set_bit(i2c->base_addr + I2C_CR1, I2C_CR1_START_BIT);

  error_t err;
  while((err = i2c->result) == 1) {
    if(task_sleep_deadline(&i2c->wait, deadline, 0)) {
      i2c->result = 0;
      i2c->read_len = 0;
      i2c->write_len = 0;
      err = ERR_TIMEOUT;
      break;
    }
  }

  reg_clr_bit(i2c->base_addr + I2C_CR2, 10); // Disable xfer IRQ
  irq_permit(q);
  mutex_unlock(&i2c->mutex);
  return err;
}


static stm32_i2c_t *
stm32_i2c_create(uint32_t base_addr, uint16_t clkid)
{
  stm32_i2c_t *i2c = malloc(sizeof(stm32_i2c_t));
  i2c->base_addr = base_addr;
  printf("i2c at 0x%x\n", base_addr);
  i2c->i2c.rw = i2c_rw;

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
  }

  reg_wr(i2c->base_addr + I2C_OAR1, (1 << 14)); // Must be 1 according to DS

  const int freq_mhz = clk_get_freq(clkid) / 1000000;

  reg_wr(i2c->base_addr + I2C_CCR, freq_mhz * 5);

  reg_wr(i2c->base_addr + I2C_CR2,
         (1 << 9) |
         (1 << 8) |
         freq_mhz);

  reg_wr(i2c->base_addr + I2C_TRISE, freq_mhz + 1);

  task_waitable_init(&i2c->wait, "i2c");
  mutex_init(&i2c->mutex, "i2cmtx");
  return i2c;
}
