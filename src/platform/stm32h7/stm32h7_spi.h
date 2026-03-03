#pragma once

#include <mios/io.h>

#define SPI_CR1    0x00
#define SPI_CR2    0x04
#define SPI_CFG1   0x08
#define SPI_CFG2   0x0c
#define SPI_IER    0x10
#define SPI_SR     0x14
#define SPI_IFCR   0x18
#define SPI_TXDR   0x20
#define SPI_RXDR   0x30
#define SPI_I2SCFGR   0x50

#define SPI1_BASE 0x40013000
#define SPI2_BASE 0x40003800
#define SPI3_BASE 0x40003c00
#define SPI4_BASE 0x40013400
#define SPI5_BASE 0x40015000
#define SPI6_BASE 0x58001400

extern int stm32h7_spi_invalid_af_for_pin; // never defined

spi_t *stm32h7_spi_create_unit(unsigned int instance);

static inline int __attribute__((always_inline))
stm32h7_spi_pin_af(unsigned int instance, gpio_t pin)
{
  switch(instance) {
  case 1:
    switch(pin) {
    case GPIO_PA(5): case GPIO_PA(6): case GPIO_PA(7):
    case GPIO_PB(3): case GPIO_PB(4): case GPIO_PB(5):
    case GPIO_PD(7):
    case GPIO_PG(9): case GPIO_PG(11):
      return 5;
    }
    break;
  case 2:
    switch(pin) {
    case GPIO_PA(9):  case GPIO_PA(12):
    case GPIO_PB(9):  case GPIO_PB(10): case GPIO_PB(13):
    case GPIO_PB(14): case GPIO_PB(15):
    case GPIO_PC(1):  case GPIO_PC(2):  case GPIO_PC(3):
    case GPIO_PD(3):
      return 5;
    }
    break;
  case 3:
    switch(pin) {
    case GPIO_PB(3): case GPIO_PB(4):
    case GPIO_PC(10): case GPIO_PC(11): case GPIO_PC(12):
      return 6;
    case GPIO_PB(2): case GPIO_PB(5):
      return 7;
    case GPIO_PD(6):
      return 5;
    }
    break;
  case 4:
    switch(pin) {
    case GPIO_PE(2):  case GPIO_PE(4):  case GPIO_PE(5):
    case GPIO_PE(6):  case GPIO_PE(11): case GPIO_PE(12):
    case GPIO_PE(13): case GPIO_PE(14):
      return 5;
    }
    break;
  case 5:
    switch(pin) {
    case GPIO_PF(6): case GPIO_PF(7): case GPIO_PF(8):
    case GPIO_PF(9): case GPIO_PF(11):
    case GPIO_PH(6):
      return 5;
    }
    break;
  case 6:
    switch(pin) {
    case GPIO_PA(0):
    case GPIO_PG(8):  case GPIO_PG(12):
    case GPIO_PG(13): case GPIO_PG(14):
      return 5;
    case GPIO_PA(15):
      return 7;
    case GPIO_PA(4): case GPIO_PA(5): case GPIO_PA(6):
    case GPIO_PA(7):
    case GPIO_PB(3): case GPIO_PB(4): case GPIO_PB(5):
    case GPIO_PC(12):
      return 8;
    }
    break;
  }
  return stm32h7_spi_invalid_af_for_pin;
}

static inline spi_t * __attribute__((always_inline))
stm32h7_spi_create(unsigned int instance, gpio_t clk, gpio_t miso,
                    gpio_t mosi, gpio_output_speed_t speed)
{
  gpio_conf_af(clk,  stm32h7_spi_pin_af(instance, clk),
               GPIO_PUSH_PULL, speed, GPIO_PULL_NONE);
  if(miso != GPIO_UNUSED)
    gpio_conf_af(miso, stm32h7_spi_pin_af(instance, miso),
                 GPIO_OPEN_DRAIN, speed, GPIO_PULL_UP);
  gpio_conf_af(mosi, stm32h7_spi_pin_af(instance, mosi),
               GPIO_PUSH_PULL, speed, GPIO_PULL_NONE);
  return stm32h7_spi_create_unit(instance);
}
