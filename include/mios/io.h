#pragma once

#include <stddef.h>
#include <stdint.h>
#include <sys/queue.h>

#include "error.h"

struct iovec;

typedef enum {
  GPIO_PULL_NONE = 0,
  GPIO_PULL_UP = 1,
  GPIO_PULL_DOWN = 2,
} gpio_pull_t;

typedef enum {
  GPIO_PUSH_PULL = 0,
  GPIO_OPEN_DRAIN = 1,
} gpio_output_type_t;


typedef enum {
  GPIO_SPEED_LOW       = 0,
  GPIO_SPEED_MID       = 1,
  GPIO_SPEED_HIGH      = 2,
  GPIO_SPEED_VERY_HIGH = 3,
} gpio_output_speed_t;

typedef enum {
  GPIO_FALLING_EDGE    = 0x1,
  GPIO_RISING_EDGE     = 0x2,
  GPIO_BOTH_EDGES      = 0x3,
} gpio_edge_t;


#include "io_types.h"

// I2C

typedef struct i2c {

  error_t (*rwv)(struct i2c *bus, uint8_t addr,
                 const struct iovec *txiov,
                 const struct iovec *rxiov,
                 size_t count);

} i2c_t;


error_t i2c_read_u8(i2c_t *i2c, uint8_t addr, uint8_t reg, uint8_t *u8);

error_t i2c_write_u8(i2c_t *i2c, uint8_t addr, uint8_t reg, uint8_t u8);

__attribute__((access(write_only, 4, 5)))
error_t i2c_read_bytes(i2c_t *i2c, uint8_t addr, uint8_t reg,
                       uint8_t *u8, size_t len);

__attribute__((access(write_only, 5, 6), access(read_only, 3, 4)))
error_t i2c_rw(struct i2c *bus, uint8_t addr,
               const uint8_t *write, size_t write_len,
               uint8_t *read, size_t read_len);

i2c_t *i2c_get_bus(unsigned int bus_id);

// SPI

typedef struct spi {
  error_t (*rw)(struct spi *bus, const uint8_t *tx, uint8_t *rx, size_t len,
                gpio_t nss, int config);

  __attribute__((access(read_only, 2, 4)))
  error_t (*rwv)(struct spi *bus,
                 const struct iovec *txiov,
                 const struct iovec *rxiov,
                 size_t count, gpio_t nss, int config);
  error_t (*rw_locked)(struct spi *bus, const uint8_t *tx, uint8_t *rx,
                       size_t len, gpio_t nss, int mode);
  void (*lock)(struct spi *bus, int acquire);

#define SPI_CPOL 0x2
#define SPI_CPHA 0x1
  int (*get_config)(struct spi *bus, int clock_flags, int baudrate);

} spi_t;

// GPIO implementations provided by platform (or stubbed out)
// Typically, memory mapped, fast and available from all IRQ levels

void gpio_conf_input(gpio_t gpio, gpio_pull_t pull);

void gpio_conf_output(gpio_t gpio, gpio_output_type_t type,
                      gpio_output_speed_t speed, gpio_pull_t pull);

void gpio_set_output(gpio_t gpio, int on);

int gpio_get_input(gpio_t gpio);

void gpio_conf_af(gpio_t gpio, int af, gpio_output_type_t type,
                  gpio_output_speed_t speed, gpio_pull_t pull);

void gpio_conf_irq(gpio_t gpio, gpio_pull_t pull,
                   void (*cb)(void *arg), void *arg,
                   gpio_edge_t edge, int level);

void gpio_conf_analog(gpio_t gpio, gpio_pull_t pull);

void gpio_dir_output(gpio_t gpio);

void gpio_conf_irq_edge(gpio_t gpio, gpio_edge_t edge);

void gpio_disconnect(gpio_t gpio);

// XGPIO - eXtended or eXpanded GPIO
// Higher level abstraction for GPIO. Allows GPIO
// over IO-expanders (connected over i2c, spi, etc)

struct xgpio;

typedef struct xgpio_vtable {

  error_t (*conf_input)(struct xgpio *xg, unsigned int line,
                        gpio_pull_t pull);

  error_t (*conf_output)(struct xgpio *xg, unsigned int line,
                         gpio_output_type_t type, gpio_output_speed_t speed,
                         gpio_pull_t pull, int initial_value);

  // Read a single GPIO pin (one bit)
  error_t (*set)(struct xgpio *xg, unsigned int line, int on);

  error_t (*get)(struct xgpio *xg, unsigned int gpio, int *status);

  // Return 1 if port is input, 0 if output
  int (*get_mode)(struct xgpio *xg, unsigned int line);

  error_t (*conf_irq)(struct xgpio *xg, unsigned int line,
                      gpio_pull_t pull,
                      void (*cb)(void *arg), void *arg,
                      gpio_edge_t edge, int level);

  error_t (*refresh_shadow)(struct xgpio *xg);

  void (*irq)(struct xgpio *xg);

} xgpio_vtable_t;

typedef struct xgpio {
  const xgpio_vtable_t *vtable;
  SLIST_ENTRY(xgpio) shared_irq_link;
} xgpio_t;

// Helper struct to carry both xpgio port and pin
typedef struct {
  xgpio_t *port;
  unsigned int line;
} xgpio_pin_t;

// mmgpio implements regular memory-mapped gpio over tha xgpio API
extern xgpio_t mmgpio;

static inline error_t
xgpio_conf_input(xgpio_t *xg, unsigned int line, gpio_pull_t pull)
{
  return xg->vtable->conf_input(xg, line, pull);
}

static inline error_t
xgpio_conf_output(xgpio_t *xg, unsigned int line,
                  gpio_output_type_t type, gpio_output_speed_t speed,
                  gpio_pull_t pull, int initial_value)
{
  return xg->vtable->conf_output(xg, line, type, speed, pull, initial_value);
}

static inline error_t
xgpio_set(xgpio_t *xg, unsigned int line, int on)
{
  return xg->vtable->set(xg, line, on);
}

static inline error_t
xgpio_get(xgpio_t *xg, unsigned int line, int *status)
{
  return xg->vtable->get(xg, line, status);
}

static inline int
xgpio_get_mode(xgpio_t *xg, unsigned int line)
{
  return xg->vtable->get_mode(xg, line);
}

static inline error_t
xgpio_conf_irq(xgpio_t *xg, unsigned int line,
               gpio_pull_t pull,
               void (*cb)(void *arg), void *arg,
               gpio_edge_t edge, int level)
{
  return xg->vtable->conf_irq(xg, line, pull, cb, arg, edge, level);
}

static inline error_t
xgpio_refresh_shadow(xgpio_t *xg)
{
  return xg->vtable->refresh_shadow(xg);
}

// xpgio_pin API

static inline error_t
xgpio_conf_output_pin(const xgpio_pin_t *xp,
                      gpio_output_type_t type, gpio_output_speed_t speed,
                      gpio_pull_t pull, int initial_value)
{
  if(!xp->port)
    return ERR_NO_DEVICE;

  return xp->port->vtable->conf_output(xp->port, xp->line, type, speed, pull,
                                       initial_value);
}

static inline error_t
xgpio_set_pin(const xgpio_pin_t *xp, int on)
{
  if(!xp->port)
    return ERR_NO_DEVICE;
  return xp->port->vtable->set(xp->port, xp->line, on);
}

static inline error_t
xgpio_get_pin(const xgpio_pin_t *xp, int *status)
{
  if(!xp->port)
    return ERR_NO_DEVICE;
  return xp->port->vtable->get(xp->port, xp->line, status);
}

static inline error_t
xgpio_conf_irq_pin(const xgpio_pin_t *xp, gpio_pull_t pull,
                   void (*cb)(void *arg), void *arg,
                   gpio_edge_t edge, int level)
{
  if(!xp->port)
    return ERR_NO_DEVICE;

  return xp->port->vtable->conf_irq(xp->port, xp->line, pull, cb, arg, edge,
                                    level);
}

// xpgio_irq_mux
// Used to hook up mmgpio IRQ to one or multiple xgpio controllers
// Assumes active-low / open-drain IRQ lines

struct xgpio_irq_mux *xgpio_irq_mux_create(gpio_t gpio);

void xgpio_irq_mux_link(struct xgpio_irq_mux *m, xgpio_t *gpio);

void xgpio_irq_mux_wakeup(struct xgpio_irq_mux *m);
