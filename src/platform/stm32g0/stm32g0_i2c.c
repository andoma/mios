#include <unistd.h>
#include <stdlib.h>
#include <assert.h>
#include <stdio.h>
#include <mios/io.h>
#include <mios/mios.h>
#include <mios/task.h>

#include "irq.h"
#include "stm32g0.h"
#include "stm32g0_clk.h"

#define I2C_BASE(x)   (0x40005400 + ((x) * 0x400))

#define I2C_CR1       0x00
#define I2C_CR2       0x04
#define I2C_TIMINGR   0x10
#define I2C_TIMEOUTR  0x14
#define I2C_ISR       0x18
#define I2C_ICR       0x1c
#define I2C_RXDR      0x24
#define I2C_TXDR      0x28


typedef struct stm32g0_i2c {
  struct i2c i2c;
  uint32_t base_addr;
  task_waitable_t wait;

  error_t result;

  const uint8_t *write;
  size_t write_len;
  uint8_t *read;
  size_t read_len;

  mutex_t mutex;

  gpio_t scl;
  gpio_t sda;
  uint8_t instance;
  gpio_pull_t pull;

} stm32g0_i2c_t;


static stm32g0_i2c_t *g_i2c[2];


static void
i2c_reset(stm32g0_i2c_t *i2c)
{
  reset_peripheral(RST_I2C(i2c->instance));
  printf("i2c%d: Reset\n", i2c->instance);

  reg_wr(i2c->base_addr + I2C_CR1, 0);

  gpio_conf_af(i2c->scl, 6, GPIO_OPEN_DRAIN, GPIO_SPEED_HIGH, i2c->pull);
  gpio_conf_af(i2c->sda, 6, GPIO_OPEN_DRAIN, GPIO_SPEED_HIGH, i2c->pull);

  // 16MHz input clock -> 100kHz i2c

  int presc = 3;
  int scll = 0x13;
  int sclh = 0xf;
  int sdadel = 0x2;
  int scldel = 0x4;

  reg_wr(i2c->base_addr + I2C_TIMINGR,
         (presc << 28) |
         (scldel << 20) |
         (sdadel << 16) |
         (sclh << 8) |
         (scll));
}

static void
i2c_done(stm32g0_i2c_t *i2c, error_t result)
{
  i2c->result = result;
  reg_wr(i2c->base_addr + I2C_CR1, 0);
  task_wakeup(&i2c->wait, 0);
}


static void
i2c_irq(stm32g0_i2c_t *i2c)
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

  if(isr & 0x40 && i2c->read_len) {
    // Write cycle completed, initiate read

    reg_wr(i2c->base_addr + I2C_CR2,
           (reg_rd(i2c->base_addr + I2C_CR2) & 0x3ff) |
           (1 << 10) | // READ
           (1 << 13) | // Generate Start
           (1 << 25) | // AutoEnd
           (i2c->read_len << 16));
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

  if(i2c->result != 1) {
    // Thread is no longer waiting, cancel everything
    reg_wr(i2c->base_addr + I2C_ICR, isr);
    return;
  }

  if(isr & 0x100)
    return i2c_done(i2c, ERR_BUS_ERROR);
  if(isr & 0x200)
    return i2c_done(i2c, ERR_ARBITRATION_LOST);

  return i2c_done(i2c, ERR_BAD_STATE);
}





static error_t
i2c_rw(i2c_t *d, uint8_t addr, const uint8_t *write, size_t write_len,
       uint8_t *read, size_t read_len)
{
  stm32g0_i2c_t *i2c = (stm32g0_i2c_t *)d;

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
    if(task_sleep_deadline(&i2c->wait, deadline, 0)) {
      i2c->result = 0;
      i2c->read_len = 0;
      i2c->write_len = 0;
      irq_permit(q);
      i2c_reset(i2c);
      mutex_unlock(&i2c->mutex);
      return ERR_TIMEOUT;
    }
  }

  irq_permit(q);
  mutex_unlock(&i2c->mutex);
  return r;
}



void irq_23(void) { i2c_irq(g_i2c[0]); }
void irq_24(void) { i2c_irq(g_i2c[1]); }


i2c_t *
stm32g0_i2c_create(int instance, gpio_t scl, gpio_t sda, gpio_pull_t pull)
{
  if(instance < 1 || instance > 2) {
    panic("i2c: Invalid instance %d", instance);
  }

  clk_enable(CLK_I2C(instance));


  stm32g0_i2c_t *d = malloc(sizeof(stm32g0_i2c_t));
  d->instance = instance;
  d->scl = scl;
  d->sda = sda;
  d->pull = pull;
  d->i2c.rw = i2c_rw;

  instance--;
  d->base_addr = I2C_BASE(instance);
  task_waitable_init(&d->wait, "i2c");
  mutex_init(&d->mutex, "i2clock");

  irq_enable(24, IRQ_LEVEL_IO);
  g_i2c[instance] = d;

  i2c_reset(d);

  return &d->i2c;
}
