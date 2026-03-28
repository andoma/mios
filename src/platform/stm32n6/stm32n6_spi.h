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

#define SPI1_BASE 0x52003000
#define SPI2_BASE 0x50003800
#define SPI3_BASE 0x50003c00
#define SPI4_BASE 0x52003400
#define SPI5_BASE 0x52005000
#define SPI6_BASE 0x56001400

spi_t *stm32n6_spi_create_unit(unsigned int instance);

extern int stm32n6_spi_invalid_af_for_pin; // never defined — link error on bad pin

static inline int __attribute__((always_inline))
stm32n6_spi_pin_af(unsigned int instance, gpio_t pin)
{
  switch(instance) {
  case 1:
    switch(pin) {
    case GPIO_PA(5):              // SCK
    case GPIO_PA(6):              // MISO
    case GPIO_PA(7):              // MOSI
    case GPIO_PA(15):             // NSS
    case GPIO_PB(4):              // MISO
    case GPIO_PB(5):              // MOSI
    case GPIO_PB(8):              // MISO
    case GPIO_PC(0):              // SCK
    case GPIO_PC(3):              // MOSI
    case GPIO_PC(5):              // NSS
    case GPIO_PF(7):              // SCK
      return 5;
    }
    break;
  case 2:
    switch(pin) {
    case GPIO_PA(9):              // SCK
    case GPIO_PA(11):             // NSS
    case GPIO_PA(12):             // SCK
    case GPIO_PB(10):             // SCK
    case GPIO_PC(1):              // NSS
    case GPIO_PC(4):              // MISO
    case GPIO_PD(2):              // MOSI
    case GPIO_PD(6):              // MISO
    case GPIO_PD(7):              // MOSI
    case GPIO_PD(11):             // MISO
    case GPIO_PF(2):              // SCK
    case GPIO_PG(8):              // MOSI
    case GPIO_PG(10):             // SCK
    case GPIO_PP(8):              // MISO
    case GPIO_PP(9):              // MOSI
      return 5;
    }
    break;
  case 3:
    switch(pin) {
    case GPIO_PB(4):              // MISO
    case GPIO_PB(5):              // MOSI
    case GPIO_PC(0):              // SCK
    case GPIO_PC(10):             // SCK
    case GPIO_PC(11):             // MISO
    case GPIO_PC(12):             // MOSI
    case GPIO_PD(7):              // NSS
      return 6;
    }
    break;
  case 4:
    switch(pin) {
    case GPIO_PB(0):              // NSS
    case GPIO_PB(6):              // MISO
    case GPIO_PB(7):              // MOSI
    case GPIO_PE(2):              // SCK
    case GPIO_PE(4):              // NSS
    case GPIO_PE(11):             // NSS
    case GPIO_PE(12):             // SCK
    case GPIO_PE(13):             // MISO
    case GPIO_PE(14):             // MOSI
    case GPIO_PG(12):             // SCK
      return 5;
    }
    break;
  case 5:
    switch(pin) {
    case GPIO_PF(11):             // MOSI
    case GPIO_PF(12):             // MISO
    case GPIO_PF(13):             // NSS
    case GPIO_PF(14):             // MOSI
    case GPIO_PF(15):             // SCK
    case GPIO_PG(1):              // MISO
    case GPIO_PG(2):              // MOSI
    case GPIO_PG(12):             // SCK
    case GPIO_PH(5):              // SCK
    case GPIO_PH(6):              // NSS
    case GPIO_PH(7):              // MOSI
    case GPIO_PH(8):              // MISO
      return 5;
    }
    break;
  case 6:
    // SPI6 pins are split between AF5 and AF8
    switch(pin) {
    case GPIO_PB(13):             // SCK (AF5)
    case GPIO_PB(15):             // NSS (AF5)
    case GPIO_PC(12):             // SCK (AF5)
    case GPIO_PE(4):              // MISO (AF5)
    case GPIO_PG(14):             // MOSI (AF5)
      return 5;
    case GPIO_PA(5):              // SCK (AF8)
    case GPIO_PA(6):              // MISO (AF8)
    case GPIO_PA(7):              // MOSI (AF8)
    case GPIO_PB(4):              // MISO (AF8)
    case GPIO_PB(5):              // MOSI (AF8)
    case GPIO_PC(0):              // SCK (AF8)
    case GPIO_PF(4):              // NSS (AF8)
      return 8;
    case GPIO_PA(15):             // NSS (AF7)
      return 7;
    }
    break;
  }
  return stm32n6_spi_invalid_af_for_pin;
}

static inline spi_t * __attribute__((always_inline))
stm32n6_spi_create(unsigned int instance, gpio_t clk, gpio_t miso,
                    gpio_t mosi, gpio_output_speed_t speed)
{
  gpio_conf_af(clk, stm32n6_spi_pin_af(instance, clk),
               GPIO_PUSH_PULL, speed, GPIO_PULL_NONE);
  if(miso != GPIO_UNUSED)
    gpio_conf_af(miso, stm32n6_spi_pin_af(instance, miso),
                 GPIO_PUSH_PULL, speed, GPIO_PULL_NONE);
  gpio_conf_af(mosi, stm32n6_spi_pin_af(instance, mosi),
               GPIO_PUSH_PULL, speed, GPIO_PULL_NONE);
  return stm32n6_spi_create_unit(instance);
}
