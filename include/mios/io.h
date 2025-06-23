#pragma once

#include <stddef.h>
#include <stdint.h>

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

// Indirect GPIO

struct indirect_gpio;

typedef struct gpio_vtable {

  error_t (*conf_input)(struct indirect_gpio *ig, unsigned int line,
                        gpio_pull_t pull);

  error_t (*conf_output)(struct indirect_gpio *ig, unsigned int line,
                         gpio_output_type_t type, gpio_output_speed_t speed,
                         gpio_pull_t pull, int initial_value);

  // Read a single GPIO pin (one bit)
  error_t (*set_pin)(struct indirect_gpio *ig, unsigned int line, int on);

  error_t (*get_pin)(struct indirect_gpio *ig, unsigned int gpio,
                     int *status);

  // Read a full port (number of pins depends on the pin multiplex)
  error_t (*get_port)(struct indirect_gpio *ig, unsigned int port,
                      uint32_t *pins);

  // Return 1 if port is input, 0 if output
  int (*get_mode)(struct indirect_gpio *ig, unsigned int line);

  error_t (*refresh_shadow)(struct indirect_gpio *ig);

} gpio_vtable_t;

typedef struct indirect_gpio {
  const gpio_vtable_t *vtable;
} indirect_gpio_t;

static inline error_t
indirect_gpio_conf_input(indirect_gpio_t *ig, unsigned int line,
                         gpio_pull_t pull)
{
  return ig->vtable->conf_input(ig, line, pull);
}

static inline error_t
indirect_gpio_conf_output(indirect_gpio_t *ig, unsigned int line,
                          gpio_output_type_t type, gpio_output_speed_t speed,
                          gpio_pull_t pull, int initial_value)
{
  return ig->vtable->conf_output(ig, line, type, speed, pull, initial_value);
}

static inline error_t
indirect_gpio_set_pin(indirect_gpio_t *ig, unsigned int line, int on)
{
  return ig->vtable->set_pin(ig, line, on);
}

static inline error_t
indirect_gpio_get_pin(indirect_gpio_t *ig, unsigned int line, int *status)
{
  return ig->vtable->get_pin(ig, line, status);
}

static inline int
indirect_gpio_get_mode(indirect_gpio_t *ig, unsigned int line)
{
  return ig->vtable->get_mode(ig, line);
}

static inline error_t
indirect_gpio_refresh_shadow(indirect_gpio_t *ig)
{
  return ig->vtable->refresh_shadow(ig);
}
