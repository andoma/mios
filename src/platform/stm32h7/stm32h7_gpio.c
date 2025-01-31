#include <mios/io.h>

#include "irq.h"
#include "stm32h7_clk.h"

#define GPIO_PORT_ADDR(x) (0x58020000 + ((x) * 0x400))

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
gpio_disconnect(gpio_t gpio)
{
  gpio_conf_input(gpio, GPIO_PULL_NONE);
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
static uint8_t gpio_irq_level[7];

#define EXTI_BASE        0x58000000

#define EXTI_RTSR1       (EXTI_BASE + 0x00)
#define EXTI_FTSR1       (EXTI_BASE + 0x04)
#define EXTI_CPUIMR1     (EXTI_BASE + 0x80)
#define EXTI_CPUPR1      (EXTI_BASE + 0x88)

#define SYSCFG_BASE 0x58000400

#define SYSCFG_EXTICR(x) (SYSCFG_BASE + 0x8 + (4 * x))

void
gpio_conf_irq(gpio_t gpio, gpio_pull_t pull, void (*cb)(void *arg), void *arg,
              gpio_edge_t edge, int level)
{
  clk_enable(CLK_SYSCFG);

  const int port = gpio >> 4;
  const int bit = gpio & 0xf;
  const int icr = bit >> 2;
  const int slice = bit & 3;


  if(gpio_irqs[bit].cb)
    panic("GPIO-IRQ for line %d already in use", bit);

  gpio_conf_input(gpio, pull);

  gpio_irqs[bit].cb  = cb;
  gpio_irqs[bit].arg = arg;

  int q = irq_forbid(IRQ_LEVEL_SCHED);

  if(edge & GPIO_FALLING_EDGE)
    reg_set_bit(EXTI_FTSR1, bit);
  else
    reg_clr_bit(EXTI_FTSR1, bit);

  if(edge & GPIO_RISING_EDGE)
    reg_set_bit(EXTI_RTSR1, bit);
  else
    reg_clr_bit(EXTI_RTSR1, bit);

  reg_set_bit(EXTI_CPUIMR1, bit);

  reg_set_bits(SYSCFG_EXTICR(icr), slice * 4, 4, port);

  irq_permit(q);

  int irq;
  int group;
  if(bit < 5) {
    irq = bit + 6;
    group = bit;
  } else if(bit < 10) {
    irq = 23;
    group = 5;
  } else {
    irq = 40;
    group = 6;
  }

  if(gpio_irq_level[group] && gpio_irq_level[group] != level) {
    panic("IRQ level conflict for group %d", group);
  }

  gpio_irq_level[group] = level;
  irq_enable(irq, level);
}



static void __attribute__((noinline))
gpio_irq(int line)
{
  reg_wr(EXTI_CPUPR1, 1 << line);
  gpio_irqs[line].cb(gpio_irqs[line].arg);
}



void
irq_6(void)
{
  gpio_irq(0);
}

void
irq_7(void)
{
  gpio_irq(1);
}

void
irq_8(void)
{
  gpio_irq(2);
}

void
irq_9(void)
{
  gpio_irq(3);
}

void
irq_10(void)
{
  gpio_irq(4);
}

void
irq_23(void)
{
  const uint32_t pr = reg_rd(EXTI_CPUPR1);
  for(int i = 5; i <= 9; i++)
    if((1 << i) & pr)
      gpio_irq(i);
}

void
irq_40(void)
{
  const uint32_t pr = reg_rd(EXTI_CPUPR1);
  for(int i = 10; i <= 15; i++)
    if((1 << i) & pr)
      gpio_irq(i);
}
