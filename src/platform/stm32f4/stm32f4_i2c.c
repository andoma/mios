#include <unistd.h>
#include <stdlib.h>
#include <assert.h>
#include <stdio.h>
#include <mios/io.h>
#include <mios/mios.h>
#include <mios/task.h>

#include "irq.h"
#include "stm32f4_reg.h"
#include "stm32f4_clk.h"

#define NAME "stm32f4_i2c"

#define I2C_BASE(x)   (0x40005400 + ((x) * 0x400))

#define I2C_CR1 0x00
#define I2C_CR2 0x04
#define I2C_DR  0x10
#define I2C_SR1 0x14
#define I2C_SR2 0x18
#define I2C_CCR 0x1c

#define I2C_CR1_START (1 << 8)
#define I2C_CR1_STOP  (1 << 9)
#if 0


static uint32_t i2c_addr;
static uint32_t i2c_buf[16];
static uint32_t i2c_len;
static uint32_t i2c_ptr;

// I2C1 Event
void
irq_31(void)
{
  uint32_t sr1 = reg_rd(I2C_BASE(0) + I2C_SR1);
  printf("irq31 sr:%x p:%d a:%x\n", sr1, i2c_ptr, i2c_addr);
  if(sr1 & 1) {
    reg_wr(I2C_BASE(0) + I2C_DR, i2c_addr);
    return;
  }
  if(sr1 & 2) {
    reg_rd(I2C_BASE(0) + I2C_SR2);
    if(i2c_addr & 1) {
      // READ
      return;
    } else {
      // WRITE
    }
  }


  if(sr1 & 0x80) {
    if(i2c_ptr == i2c_len) {
      reg_set(I2C_BASE(0) + I2C_CR1, I2C_CR1_STOP);
    } else {
      reg_wr(I2C_BASE(0) + I2C_DR, i2c_buf[i2c_ptr]);
      i2c_ptr++;
    }
  }

  if(sr1 & 0x40) {
    if(i2c_ptr == i2c_len) {
      reg_set(I2C_BASE(0) + I2C_CR1, I2C_CR1_STOP);
    } else {
      i2c_buf[i2c_ptr] = reg_rd(I2C_BASE(0) + I2C_DR);
      i2c_ptr++;
    }
  }
}


// I2C1 Error
void
irq_32(void)
{
  uint32_t sr = reg_rd(I2C_BASE(0) + I2C_SR1);
  printf("irq_32 sr:%x\n", sr);
  if(sr & 0x400) {
    printf("NACK happened\n");
    reg_clr(I2C_BASE(0) + I2C_SR1, 0x400);
    return;
  }
}

#endif


typedef struct stm32f4_i2c {
  struct i2c i2c;
  uint32_t base_addr;
} stm32f4_i2c_t;


static error_t
i2c_start(stm32f4_i2c_t *i2c)
{
  int x = 0;
  reg_set(i2c->base_addr + I2C_CR1, I2C_CR1_START);
  while(!(reg_rd(i2c->base_addr + I2C_SR1) & 1)) {
    x++;
    if(x == 1000000) {
      printf("i2c: start timeout\n");
      return ERR_TIMEOUT;
    }
  }
  return ERR_OK;
}


static error_t
i2c_stop(stm32f4_i2c_t *i2c)
{
  int x = 0;
  reg_set(i2c->base_addr + I2C_CR1, I2C_CR1_STOP);
  while(reg_rd(i2c->base_addr + I2C_SR1) & 2) {
    x++;
    if(x == 1000000) {
      printf("i2c: stop timeout\n");
      return ERR_TIMEOUT;
    }
  }
  return ERR_OK;
}


static error_t
i2c_write(stm32f4_i2c_t *i2c, uint8_t v)
{
  int x = 0;
  reg_wr(i2c->base_addr + I2C_DR, v);
  while(!(reg_rd(i2c->base_addr + I2C_SR1) & 0x80)) {
    x++;
    if(x == 1000000) {
      printf("i2c: write timeout\n");
      return ERR_TIMEOUT;
    }
  }
  return ERR_OK;
}

static error_t
i2c_read(stm32f4_i2c_t *i2c, int ack, uint8_t *ptr)
{
  int x = 0;
  if(ack)
    reg_set_bit(i2c->base_addr + I2C_CR1, 10);
  else
    reg_clr_bit(i2c->base_addr + I2C_CR1, 10);

  while(!(reg_rd(i2c->base_addr + I2C_SR1) & 0x40)) {
    x++;
    if(x == 1000000) {
      printf("i2c: read timeout\n");
      return ERR_TIMEOUT;
    }
  }
  *ptr = reg_rd(i2c->base_addr + I2C_DR);
  return ERR_OK;
}

static error_t
i2c_addr(stm32f4_i2c_t *i2c, uint8_t addr)
{
  int x = 0;
  reg_wr(i2c->base_addr + I2C_DR, addr);
  while(!(reg_rd(i2c->base_addr + I2C_SR1) & 0x2)) {
    x++;
    if(x == 1000000) {
      printf("i2c: addr timeout\n");
      return ERR_TIMEOUT;
    }
  }
  reg_rd(i2c->base_addr + I2C_SR2);
  return ERR_OK;
}


static error_t
i2c_rw(i2c_t *d, uint8_t addr, const uint8_t *write, size_t write_len,
       uint8_t *read, size_t read_len)
{
  stm32f4_i2c_t *i2c = (stm32f4_i2c_t *)d;
  error_t r;

  assert(write_len || read_len);

  if(write_len) {
    if((r = i2c_start(i2c)) != ERR_OK)
      return r;
    if((r = i2c_addr(i2c, addr << 1)) != ERR_OK) {
      i2c_stop(i2c);
      return r;
    }

    for(size_t i = 0; i < write_len; i++) {
      if((r = i2c_write(i2c, write[i])) != ERR_OK) {
        i2c_stop(i2c);
        return r;
      }
    }
  }

  if(read_len) {
    if((r = i2c_start(i2c)) != ERR_OK)
      return r;
    if((r = i2c_addr(i2c, addr << 1 | 1)) != ERR_OK) {
      i2c_stop(i2c);
      return r;
    }
    for(size_t i = 0; i < read_len; i++) {
      if((r = i2c_read(i2c, i + 1 < read_len, read + i)) != ERR_OK) {
        i2c_stop(i2c);
        return r;
      }
    }
  }
  i2c_stop(i2c);
  return ERR_OK;
}


i2c_t *
stm32f4_i2c_create(int instance, gpio_t scl, uint32_t sda_cfg, gpio_pull_t pull)
{
  if(instance < 1 || instance > 3) {
    panic("%s: Invalid instance %d", NAME, instance);
  }

  const int sda = sda_cfg & 0xff;
  const int sda_af = sda_cfg >> 8;
  if(!sda_af)
    panic("i2c: Bad sda config");

  instance--;

  clk_enable(CLK_I2C(instance));

  // If bus seems to be stuck, toggle SCL until SDA goes high again
  gpio_conf_input(sda, GPIO_PULL_UP);
  gpio_conf_output(scl, GPIO_OPEN_DRAIN, GPIO_SPEED_HIGH, GPIO_PULL_NONE);

  int c = 0;
  while(1) {
    int d = gpio_get_input(sda);
    if(d)
      break;

    c = !c;
    gpio_set_output(scl, c);

    udelay(10);
  }

  stm32f4_i2c_t *d = malloc(sizeof(stm32f4_i2c_t));
  d->i2c.rw = i2c_rw;
  d->base_addr = I2C_BASE(instance);
  reg_wr(d->base_addr + I2C_CR1, 0x0);

  gpio_conf_af(scl, 4, GPIO_OPEN_DRAIN, GPIO_SPEED_HIGH, pull);

  gpio_conf_af(sda, sda_af, GPIO_OPEN_DRAIN, GPIO_SPEED_HIGH, pull);

  udelay(100);
  reg_wr(d->base_addr + I2C_CR1, 1);
  udelay(100);
  if(reg_rd(d->base_addr + I2C_SR2) & 2) {
    reg_wr(d->base_addr + I2C_CR1, 0x8001);
    while(reg_rd(d->base_addr + I2C_SR2) & 2) {
    }
    reg_wr(d->base_addr + I2C_CR1, 0x0);
    udelay(100);
    reg_wr(d->base_addr + I2C_CR1, 0x1);
  }
  reg_wr(d->base_addr + I2C_CCR, 110);
  reg_wr(d->base_addr + I2C_CR2, 24); //  | (1 << 8) | (1 << 9));

  return &d->i2c;
}
