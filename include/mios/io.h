#pragma once

#include <stddef.h>
#include <stdint.h>

#include "error.h"



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
  error_t (*rw)(struct i2c *bus, uint8_t addr,
                const uint8_t *write, size_t write_len,
                uint8_t *read, size_t read_len);
} i2c_t;


error_t i2c_read_u8(i2c_t *i2c, uint8_t addr, uint8_t reg, uint8_t *u8);

error_t i2c_write_u8(i2c_t *i2c, uint8_t addr, uint8_t reg, uint8_t u8);

error_t i2c_read_bytes(i2c_t *i2c, uint8_t addr, uint8_t reg,
                       uint8_t *u8, size_t len);

// SPI

typedef struct spi {
  error_t (*rw)(struct spi *bus, const uint8_t *tx, uint8_t *rx, size_t len,
                gpio_t nss);
  error_t (*rw_locked)(struct spi *bus, const uint8_t *tx, uint8_t *rx,
                       size_t len, gpio_t nss);
  void (*lock)(struct spi *bus, int acquire);

} spi_t;

// GPIO implementations provided by platform (or stubbed out)

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

