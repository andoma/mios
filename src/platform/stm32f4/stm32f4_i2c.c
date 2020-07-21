#include <assert.h>
#include <stdio.h>

#include "irq.h"
#include "stm32f4.h"
#include "gpio.h"
#include "mios.h"
#include "task.h"

#include "i2c.h"

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


struct i2c {
  uint32_t base_addr;
};


static error_t
i2c_start(i2c_t *i2c)
{
  int x = 0;
  reg_set(i2c->base_addr + I2C_CR1, I2C_CR1_START);
  while(!(reg_rd(i2c->base_addr + I2C_SR1) & 1)) {
    x++;
    if(x == 1000000)
      return ERR_TIMEOUT;
  }
  return ERR_OK;
}


static error_t
i2c_stop(i2c_t *i2c)
{
  int x = 0;
  reg_set(i2c->base_addr + I2C_CR1, I2C_CR1_STOP);
  while(reg_rd(i2c->base_addr + I2C_SR1) & 2) {
    x++;
    if(x == 1000000)
      return ERR_TIMEOUT;
  }
  return ERR_OK;
}


static error_t
i2c_write(i2c_t *i2c, uint8_t v)
{
  int x = 0;
  reg_wr(i2c->base_addr + I2C_DR, v);
  while(!(reg_rd(i2c->base_addr + I2C_SR1) & 0x80)) {
    x++;
    if(x == 1000000)
      return ERR_TIMEOUT;
  }
  return ERR_OK;
}

static error_t
i2c_read(i2c_t *i2c, int ack, uint8_t *ptr)
{
  int x = 0;
  if(ack)
    reg_set(i2c->base_addr + I2C_CR1, 0x400);
  else
    reg_clr(i2c->base_addr + I2C_CR1, 0x400);

  while(!(reg_rd(i2c->base_addr + I2C_SR1) & 0x40)) {
    x++;
    if(x == 1000000)
      return ERR_TIMEOUT;
  }
  *ptr = reg_rd(i2c->base_addr + I2C_DR);
  return ERR_OK;
}

static error_t
i2c_addr(i2c_t *i2c, uint8_t addr)
{
  int x = 0;
  reg_wr(i2c->base_addr + I2C_DR, addr);
  while(!(reg_rd(i2c->base_addr + I2C_SR1) & 0x2)) {
    x++;
    if(x == 1000000)
      return ERR_TIMEOUT;
  }
  reg_rd(i2c->base_addr + I2C_SR2);
  return ERR_OK;
}


error_t
i2c_rw(i2c_t *i2c, uint8_t addr, const uint8_t *write, size_t write_len,
       uint8_t *read, size_t read_len)
{
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


void
i2c_init(struct i2c *i2c, uint32_t base_addr)
{
  i2c->base_addr = base_addr;

  reg_wr(base_addr + I2C_CCR, 210);
  reg_wr(base_addr + I2C_CR2, 42); //  | (1 << 8) | (1 << 9));
  reg_wr(base_addr + I2C_CR1, 0x1);
}



#if 0



#include "drivers/ms5611.h"

static void *
i2c_test(void *arg)
{
  i2c_t *i2c = &i2c1;
  error_t err;
  ms5611_t *dev;

  err = ms5611_create(i2c, &dev);
  if(err) {
    printf("i2c_test failed to create device: %d\n", err);
    return NULL;
  }

  while(1) {
    ms5611_value_t values;

    err = ms5611_read(dev, &values);
    if(err) {
      printf("i2c_test failed to read values: %d\n", err);
      return NULL;
    }

    printf("lol temp: %f  pressure: %f\n", values.temp, values.pressure);
  }
  return NULL;

}

static void __attribute__((constructor(200)))
i2c_init(void)
{
  reg_set(RCC_AHB1ENR, 0x02);     // CLK ENABLE: GPIOB
  reg_set(RCC_APB1ENR, 1 << 21);  // CLK ENABLE: I2C1

  // Set GPIO ports in open drain
  reg_set_bits(GPIO_OTYPER(GPIO_B), 6, 1, 1);
  reg_set_bits(GPIO_OTYPER(GPIO_B), 7, 1, 1);

  // Configure PB6, PB7 for I2C (Alternative Function 4)
  gpio_conf_af(GPIO_B, 6, 4, GPIO_SPEED_HIGH, GPIO_PULL_NONE);
  gpio_conf_af(GPIO_B, 7, 4, GPIO_SPEED_HIGH, GPIO_PULL_NONE);

  //  irq_enable(31, IRQ_LEVEL_IO);
  //  irq_enable(32, IRQ_LEVEL_IO);

  reg_wr(I2C_BASE(0) + I2C_CCR, 210);
  reg_wr(I2C_BASE(0) + I2C_CR2, 42); //  | (1 << 8) | (1 << 9));
  reg_wr(I2C_BASE(0) + I2C_CR1, 0x1);

  if(1)
    task_create(i2c_test, NULL, 1024, "i2c", TASK_FPU);

}
#endif
