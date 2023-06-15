#include <mios/io.h>

#include <mios/mios.h>

#include "stm32g0_reg.h"
#include "stm32g0_clk.h"
#include "stm32g0_pwr.h"
#include "irq.h"

#define GPIO_PORT_ADDR(x) (0x50000000 + ((x) * 0x400))

#define GPIO_MODER(x)   (GPIO_PORT_ADDR(x) + 0x00)
#define GPIO_OTYPER(x)  (GPIO_PORT_ADDR(x) + 0x04)
#define GPIO_OSPEEDR(x) (GPIO_PORT_ADDR(x) + 0x08)
#define GPIO_PUPDR(x)   (GPIO_PORT_ADDR(x) + 0x0c)
#define GPIO_IDR(x)     (GPIO_PORT_ADDR(x) + 0x10)
#define GPIO_ODR(x)     (GPIO_PORT_ADDR(x) + 0x14)
#define GPIO_BSRR(x)    (GPIO_PORT_ADDR(x) + 0x18)
#define GPIO_LCKR(x)    (GPIO_PORT_ADDR(x) + 0x1c)
#define GPIO_AFRL(x)    (GPIO_PORT_ADDR(x) + 0x20)
#define GPIO_AFRH(x)    (GPIO_PORT_ADDR(x) + 0x24)



void
gpio_conf_analog(gpio_t gpio)
{
  const int port = gpio >> 4;
  const int bit = gpio & 0xf;
  clk_enable(CLK_GPIO(port));
  int s = irq_forbid(IRQ_LEVEL_IO);
  reg_set_bits(GPIO_MODER(port), bit * 2, 2, 3);
  reg_set_bits(GPIO_PUPDR(port), bit * 2, 2, 0);
  irq_permit(s);
}


void
gpio_conf_input(gpio_t gpio, gpio_pull_t pull)
{
  const int port = gpio >> 4;
  const int bit = gpio & 0xf;

  int s = irq_forbid(IRQ_LEVEL_IO);
  clk_enable(CLK_GPIO(port));
  reg_set_bits(GPIO_MODER(port), bit * 2, 2, 0);
  reg_set_bits(GPIO_PUPDR(port), bit * 2, 2, pull);
  irq_permit(s);
}



void
gpio_conf_output(gpio_t gpio,
                 gpio_output_type_t type,
                 gpio_output_speed_t speed,
                 gpio_pull_t pull)
{
  const int port = gpio >> 4;
  const int bit = gpio & 0xf;

  int s = irq_forbid(IRQ_LEVEL_IO);
  clk_enable(CLK_GPIO(port));
  reg_set_bits(GPIO_OTYPER(port),  bit, 1, type);
  reg_set_bits(GPIO_OSPEEDR(port), bit * 2, 2, speed);
  reg_set_bits(GPIO_PUPDR(port), bit * 2, 2, pull);
  reg_set_bits(GPIO_MODER(port), bit * 2, 2, 1);
  irq_permit(s);
}


void
gpio_dir_output(gpio_t gpio)
{
  const int port = gpio >> 4;
  const int bit = gpio & 0xf;
  reg_set_bits(GPIO_MODER(port), bit * 2, 2, 1);
}


void
gpio_conf_af(gpio_t gpio, int af, gpio_output_type_t type,
             gpio_output_speed_t speed, gpio_pull_t pull)
{
  const int port = gpio >> 4;
  const int bit = gpio & 0xf;

  int s = irq_forbid(IRQ_LEVEL_IO);
  clk_enable(CLK_GPIO(port));

  reg_set_bits(GPIO_OTYPER(port),  bit, 1, type);
  reg_set_bits(GPIO_OSPEEDR(port), bit * 2, 2, speed);

  if(bit < 8) {
    reg_set_bits(GPIO_AFRL(port), bit * 4, 4, af);
  } else {
    reg_set_bits(GPIO_AFRH(port), (bit - 8) * 4, 4, af);
  }

  reg_set_bits(GPIO_PUPDR(port), bit * 2, 2, pull);

  reg_set_bits(GPIO_MODER(port), bit * 2, 2, 2);
  irq_permit(s);
}

void
gpio_conf_standby(gpio_t gpio, gpio_pull_t pull)
{
  const int port = gpio >> 4;
  const int bit = gpio & 0xf;

  clk_enable(CLK_PWR);

  reg_clr_bit(PWR_PUCRx(port), bit);
  reg_clr_bit(PWR_PDCRx(port), bit);

  if(pull == GPIO_PULL_UP) {
    reg_set_bit(PWR_PUCRx(port), bit);
  } else if(pull == GPIO_PULL_DOWN) {
    reg_set_bit(PWR_PDCRx(port), bit);
  }
}



void
gpio_set_output(gpio_t gpio, int on)
{
  const int port = gpio >> 4;
  const int bit = gpio & 0xf;

  reg_set(GPIO_BSRR(port), 1 << (bit + !on * 16));
}


int
gpio_get_input(gpio_t gpio)
{
  const int port = gpio >> 4;
  const int bit = gpio & 0xf;

  uint32_t idr = reg_rd(GPIO_IDR(port));

  return !!((1 << bit) & idr);
}



typedef struct {
  void (*cb)(void *arg);
  void *arg;
} gpio_irq_t;

static gpio_irq_t gpio_irqs[16];
static uint8_t gpio_irq_level[3];


#define EXTI_RTSR1        0x40021800
#define EXTI_FTSR1        0x40021804
#define EXTI_RPR1         0x4002180c
#define EXTI_FPR1         0x40021810

#define EXTI_EXTICR(x)   (0x40021860 + 4 * (x))
#define EXTI_IMR          0x40021880


void
gpio_conf_irq_edge(gpio_t gpio, gpio_edge_t edge)
{
  const int bit = gpio & 0xf;

  if(edge & GPIO_FALLING_EDGE)
    reg_set_bit(EXTI_FTSR1, bit);
  else
    reg_clr_bit(EXTI_FTSR1, bit);

  if(edge & GPIO_RISING_EDGE)
    reg_set_bit(EXTI_RTSR1, bit);
  else
    reg_clr_bit(EXTI_RTSR1, bit);
}


void
gpio_conf_irq(gpio_t gpio, gpio_pull_t pull, void (*cb)(void *arg), void *arg,
              gpio_edge_t edge, int level)
{
  const int port = gpio >> 4;
  const int bit = gpio & 0xf;

  if(gpio_irqs[bit].cb)
    panic("GPIO-IRQ for line %d already in use", bit);

  gpio_conf_input(gpio, pull);

  gpio_irqs[bit].cb  = cb;
  gpio_irqs[bit].arg = arg;

  const int icr = bit >> 2;
  const int slice = bit & 3;

  if(edge & GPIO_FALLING_EDGE)
    reg_set_bit(EXTI_FTSR1, bit);
  else
    reg_clr_bit(EXTI_FTSR1, bit);

  if(edge & GPIO_RISING_EDGE)
    reg_set_bit(EXTI_RTSR1, bit);
  else
    reg_clr_bit(EXTI_RTSR1, bit);

  reg_set_bit(EXTI_IMR, bit);

  int s = irq_forbid(IRQ_LEVEL_SCHED);
  reg_set_bits(EXTI_EXTICR(icr), slice * 8, 8, port);
  irq_permit(s);

  int irq;
  int group;
  if(bit < 2) {
    irq = 5;
    group = 0;
  } else if(bit < 4) {
    irq = 6;
    group = 1;
  } else {
    irq = 7;
    group = 2;
  }

  if(gpio_irq_level[group] && gpio_irq_level[group] != level) {
    panic("IRQ level conflict for group %d", group);
  }

  gpio_irq_level[group] = level;
  irq_enable(irq, level);
}

static void __attribute__((noinline))
gpio_irq(int start, int end)
{
  const uint32_t rpr = reg_rd(EXTI_RPR1);
  reg_wr(EXTI_RPR1, rpr);
  const uint32_t fpr = reg_rd(EXTI_FPR1);
  reg_wr(EXTI_FPR1, fpr);

  const uint32_t pr = rpr | fpr;
  for(int i = start; i <= end; i++) {
    if((1 << i) & pr) {
      gpio_irqs[i].cb(gpio_irqs[i].arg);
    }
  }
}



void
irq_5(void)
{
  gpio_irq(0,1);
}

void
irq_6(void)
{
  gpio_irq(2,3);
}

void
irq_7(void)
{
  gpio_irq(4,15);
}
