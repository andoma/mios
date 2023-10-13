#include <mios/io.h>

#include "irq.h"
#include "stm32f4_reg.h"
#include "stm32f4_clk.h"

#include "platform/stm32/stm32_i2c_v1.c"

#define I2C_BASE(x)   (0x40005400 + ((x) * 0x400))

static stm32_i2c_t *g_i2c[3];

i2c_t *
i2c_get_bus(unsigned int bus_id)
{
  bus_id--;
  if(bus_id > ARRAYSIZE(g_i2c))
    return NULL;
  return (i2c_t *)g_i2c[bus_id];
}

void
irq_31(void)
{
  i2c_irq_ev(g_i2c[0]);
}

void
irq_32(void)
{
  i2c_irq_er(g_i2c[0]);
}

void
irq_33(void)
{
  i2c_irq_ev(g_i2c[1]);
}

void
irq_34(void)
{
  i2c_irq_er(g_i2c[1]);
}

void
irq_72(void)
{
  i2c_irq_ev(g_i2c[2]);
}

void
irq_73(void)
{
  i2c_irq_er(g_i2c[2]);
}


typedef struct {
  uint8_t instance;
  gpio_t gpio;
  uint8_t af;
  uint8_t device_mask;
} i2c_gpio_map_t;


static const i2c_gpio_map_t i2c_scl_map[] = {
  { 3, GPIO_PA(8),  4, STM32F4_05 | STM32F4_11 | STM32F4_46 },
  { 1, GPIO_PB(6),  4, STM32F4_05 | STM32F4_11 | STM32F4_46 },
  { 1, GPIO_PB(8),  4, STM32F4_05 | STM32F4_11 | STM32F4_46 },
  { 2, GPIO_PB(10), 4, STM32F4_05 | STM32F4_11 | STM32F4_46 },
  { 2, GPIO_PF(1),  4, STM32F4_05 |              STM32F4_46 },
};

static const i2c_gpio_map_t i2c_sda_map[] = {
  { 2, GPIO_PB(3),  4, STM32F4_46 },
  { 2, GPIO_PB(3),  9, STM32F4_11 },
  { 3, GPIO_PB(4),  4, STM32F4_46 },
  { 3, GPIO_PB(4),  9, STM32F4_11 },
  { 3, GPIO_PB(4),  4, STM32F4_46 },
  { 1, GPIO_PB(7),  4, STM32F4_05 |STM32F4_11 | STM32F4_46 },
  { 3, GPIO_PB(8),  9, STM32F4_11 },

  { 1, GPIO_PB(9),  4, STM32F4_05 | STM32F4_11 | STM32F4_46 },
  { 2, GPIO_PB(9),  9, STM32F4_11 },

  { 2, GPIO_PB(11), 4, STM32F4_05 | STM32F4_11 | STM32F4_46 },
  { 3, GPIO_PC(9),  4, STM32F4_05 | STM32F4_11 | STM32F4_46 },
  { 2, GPIO_PC(12), 4, STM32F4_46 },
  { 2, GPIO_PF(0),  4, STM32F4_05 | STM32F4_46 },
};


extern unsigned int stm32f4_device_mask;


static int
lookup_af(const i2c_gpio_map_t map[], size_t size,
          int instance, gpio_t gpio, const char *name)
{
  for(size_t i = 0; i < size; i++) {
    if(map[i].instance == instance &&
       map[i].gpio == gpio &&
       map[i].device_mask & stm32f4_device_mask) {
      return map[i].af;
    }
  }
  panic("i2c: Unable to find %s AF (instance:%d gpio:0x%x)", name,
        instance, gpio);
}



i2c_t *
stm32f4_i2c_create(int instance, gpio_t scl, gpio_t sda, gpio_pull_t pull,
                   gpio_output_speed_t drive_strength)
{
  if(instance < 1 || instance > 3) {
    panic("i2c: Invalid instance %d", instance);
  }

  int scl_af = lookup_af(i2c_scl_map, ARRAYSIZE(i2c_scl_map), instance, scl,
                         "scl");
  int sda_af = lookup_af(i2c_sda_map, ARRAYSIZE(i2c_sda_map), instance, sda,
                         "sda");
  instance--;

  // If bus seems to be stuck, toggle SCL until SDA goes high again
  gpio_conf_input(sda, GPIO_PULL_UP);
  gpio_conf_output(scl, GPIO_OPEN_DRAIN, drive_strength, GPIO_PULL_NONE);

  int c = 0;
  while(1) {
    int d = gpio_get_input(sda);
    if(d)
      break;

    c = !c;
    gpio_set_output(scl, c);

    udelay(10);
  }

  const uint16_t clkid = CLK_I2C(instance);
  const uint16_t rstid = RST_I2C(instance);
  clk_enable(clkid);

  gpio_conf_af(scl, scl_af, GPIO_OPEN_DRAIN, drive_strength, pull);
  gpio_conf_af(sda, sda_af, GPIO_OPEN_DRAIN, drive_strength, pull);

  stm32_i2c_t *i2c = stm32_i2c_create(I2C_BASE(instance), clkid, rstid);

  uint8_t irqbase = (const uint8_t []){31,33,72}[instance];
  irq_enable(irqbase, IRQ_LEVEL_IO);
  irq_enable(irqbase + 1, IRQ_LEVEL_IO);
  g_i2c[instance] = i2c;
  return &i2c->i2c;
}
