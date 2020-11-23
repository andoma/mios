#include <mios/io.h>
#include <mios/error.h>

error_t
i2c_read_u8(i2c_t *i2c, uint8_t addr, uint8_t reg, uint8_t *u8)
{
  return i2c->rw(i2c, addr, &reg, 1, u8, 1);
}


error_t
i2c_write_u8(i2c_t *i2c, uint8_t addr, uint8_t reg, uint8_t u8)
{
  uint8_t buf[2] = {reg, u8};
  return i2c->rw(i2c, addr, buf, 2, NULL, 0);
}


error_t
i2c_read_bytes(i2c_t *i2c, uint8_t addr, uint8_t reg,
               uint8_t *u8, size_t len)
{
  return i2c->rw(i2c, addr, &reg, 1, u8, len);
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
