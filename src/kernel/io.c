#include <mios/io.h>
#include <mios/error.h>
#include <mios/task.h>
#include <sys/uio.h>
#include <stdlib.h>

#include "irq.h"

error_t
i2c_rw(struct i2c *i2c, uint8_t addr,
       const uint8_t *write, size_t write_len,
       uint8_t *read, size_t read_len)
{
  struct iovec txiov = {(uint8_t *)write, write_len};
  struct iovec rxiov = {read, read_len};

  return i2c->rwv(i2c, addr, write ? &txiov : NULL, read ? &rxiov : NULL, 1);
}

error_t
i2c_read_u8(i2c_t *i2c, uint8_t addr, uint8_t reg, uint8_t *u8)
{
  return i2c_rw(i2c, addr, &reg, 1, u8, 1);
}


error_t
i2c_write_u8(i2c_t *i2c, uint8_t addr, uint8_t reg, uint8_t u8)
{
  uint8_t buf[2] = {reg, u8};
  return i2c_rw(i2c, addr, buf, 2, NULL, 0);
}


error_t
i2c_read_bytes(i2c_t *i2c, uint8_t addr, uint8_t reg,
               uint8_t *u8, size_t len)
{
  return i2c_rw(i2c, addr, &reg, 1, u8, len);
}

i2c_t *  __attribute__((weak))
i2c_get_bus(unsigned int bus_id)
{
  return NULL;
}


// Weak stubs for IO methods. There are overriden by the linker if
// platform specific code provides such interface.

void __attribute__((weak))
gpio_conf_input(gpio_t gpio, gpio_pull_t pull)
{
}

void __attribute__((weak))
gpio_conf_output(gpio_t gpio, gpio_output_type_t type,
                 gpio_output_speed_t speed, gpio_pull_t pull)
{

}

void __attribute__((weak))
gpio_set_output(gpio_t gpio, int on)
{

}

int __attribute__((weak))
gpio_get_input(gpio_t gpio)
{
  return 0;
}

void __attribute__((weak))
gpio_conf_irq(gpio_t gpio, gpio_pull_t pull,
              void (*cb)(void *arg), void *arg,
              gpio_edge_t edge, int level)
{

}


static error_t
mmgpio_conf_input(struct xgpio *ig, unsigned int line, gpio_pull_t pull)
{
  gpio_conf_input(line, pull);
  return 0;
}

static error_t
mmgpio_conf_output(struct xgpio *ig, unsigned int line,
                   gpio_output_type_t type, gpio_output_speed_t speed,
                   gpio_pull_t pull, int initial_value)
{
  // A workaround to deal with that gpio_conf_output() have no initial value
  gpio_conf_input(line, pull);
  gpio_set_output(line, initial_value);
  gpio_conf_output(line, type, speed, pull);
  return 0;
}


static error_t
mmgpio_set(struct xgpio *ig, unsigned int line, int on)
{
  gpio_set_output(line, on);
  return 0;
}


static error_t
mmgpio_get(struct xgpio *ig, unsigned int line, int *status)
{
  *status = gpio_get_input(line);
  return 0;
}


static error_t
mmgpio_refresh_shadow(struct xgpio *ig)
{
  return 0;
}


static error_t
mmgpio_conf_irq(struct xgpio *ig, unsigned int line,
                gpio_pull_t pull,
                void (*cb)(void *arg), void *arg,
                gpio_edge_t edge, int level)
{
  gpio_conf_irq(line, pull, cb, arg, edge, level);
  return 0;
}


static const xgpio_vtable_t mmgpio_vtable = {
  .conf_input = mmgpio_conf_input,
  .conf_output = mmgpio_conf_output,
  .set = mmgpio_set,
  .get = mmgpio_get,
  .conf_irq = mmgpio_conf_irq,
  .refresh_shadow = mmgpio_refresh_shadow
};

xgpio_t mmgpio = {
  &mmgpio_vtable
};


struct xgpio_irq_mux {
  task_waitable_t waitq;
  SLIST_HEAD(, xgpio) list;
  gpio_t gpio;
  uint8_t pending;
  uint8_t soft_wakeup;
};

static void
xgpio_irq_mux_isr(void *arg)
{
  struct xgpio_irq_mux *m = arg;
  m->pending = !gpio_get_input(m->gpio);
  task_wakeup(&m->waitq, 0);
}


__attribute__((noreturn))
static void *
xgpio_irq_mux_thread(void *arg)
{
  struct xgpio_irq_mux *m = arg;
  xgpio_t *ig;

  while(1) {

    int q = irq_forbid(IRQ_LEVEL_SWITCH);
    while(m->pending == 0 && m->soft_wakeup == 0) {
      task_sleep(&m->waitq);
    }
    m->soft_wakeup = 0;
    irq_permit(q);

    SLIST_FOREACH(ig, &m->list, shared_irq_link) {
      ig->vtable->irq(ig);
    }
  }
}


struct xgpio_irq_mux *
xgpio_irq_mux_create(gpio_t gpio)
{
  struct xgpio_irq_mux *m = calloc(1, sizeof(struct xgpio_irq_mux));

  task_waitable_init(&m->waitq, "gpio");
  m->gpio = gpio;

  gpio_conf_irq(gpio, GPIO_PULL_UP, xgpio_irq_mux_isr, m,
                GPIO_BOTH_EDGES, IRQ_LEVEL_SWITCH);

  thread_create(xgpio_irq_mux_thread, m, 0, "xgpiomux", 0, 12);
  return m;
}


void
xgpio_irq_mux_link(struct xgpio_irq_mux *m, xgpio_t *gpio)
{
  SLIST_INSERT_HEAD(&m->list, gpio, shared_irq_link);
}

void
xgpio_irq_mux_wakeup(struct xgpio_irq_mux *m)
{
  int q = irq_forbid(IRQ_LEVEL_SWITCH);
  m->soft_wakeup = 1;
  task_wakeup(&m->waitq, 0);
  irq_permit(q);
}
