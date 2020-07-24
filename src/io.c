#include <io.h>

// Weak stubs for IO methods. There are overriden by the linker if
// platform specific code provides such interface.


error_t __attribute__((weak))
i2c_rw(i2c_t *i2c, uint8_t addr,
       const uint8_t *write, size_t write_len,
       uint8_t *read, size_t read_len)
{
  return ERR_NOT_IMPLEMENTED;
}


error_t __attribute__((weak))
spi_rw(spi_t *bus, const uint8_t *tx, uint8_t *rx, size_t len, gpio_t nss)
{
  return ERR_NOT_IMPLEMENTED;
}




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
