#include <mios/io.h>

#include "irq.h"
#include "stm32n6_clk.h"

#define GPIO_PORT_ADDR(x) (0x56020000 + ((x) * 0x400))

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
gpio_conf_analog(gpio_t gpio, gpio_pull_t pull)
{
  const int port = gpio >> 4;
  const int bit = gpio & 0xf;
  clk_enable(CLK_GPIO(port));
  int s = irq_forbid(IRQ_LEVEL_IO);
  reg_set_bits(GPIO_MODER(port), bit * 2, 2, 3);
  reg_set_bits(GPIO_PUPDR(port), bit * 2, 2, pull);
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


// EXTI — External Interrupt Controller
// STM32N6 has individual IRQs per EXTI line (IRQ 20-35 for lines 0-15)
// and separate rising/falling pending registers (RPR1/FPR1).

#define EXTI_BASE        0x56025000  // AHB4

#define EXTI_RTSR1       (EXTI_BASE + 0x00)
#define EXTI_FTSR1       (EXTI_BASE + 0x04)
#define EXTI_RPR1        (EXTI_BASE + 0x0C)
#define EXTI_FPR1        (EXTI_BASE + 0x10)
#define EXTI_EXTICR(x)   (EXTI_BASE + 0x60 + (4 * (x)))
#define EXTI_IMR1        (EXTI_BASE + 0x80)

typedef struct {
  void (*cb)(void *arg);
  void *arg;
} gpio_irq_t;

static gpio_irq_t gpio_irqs[16];

void
gpio_conf_irq(gpio_t gpio, gpio_pull_t pull, void (*cb)(void *arg), void *arg,
              gpio_edge_t edge, int level)
{
  const int port = gpio >> 4;
  const int bit = gpio & 0xf;

  if(gpio_irqs[bit].cb)
    panic("GPIO-IRQ for line %d already in use", bit);

  int q = irq_forbid(IRQ_LEVEL_SCHED);

  gpio_conf_input(gpio, pull);

  gpio_irqs[bit].cb  = cb;
  gpio_irqs[bit].arg = arg;

  // Select GPIO port for this EXTI line (8-bit fields in EXTICR)
  reg_set_bits(EXTI_EXTICR(bit >> 2), (bit & 3) * 8, 8, port);

  if(edge & GPIO_FALLING_EDGE)
    reg_set_bit(EXTI_FTSR1, bit);
  else
    reg_clr_bit(EXTI_FTSR1, bit);

  if(edge & GPIO_RISING_EDGE)
    reg_set_bit(EXTI_RTSR1, bit);
  else
    reg_clr_bit(EXTI_RTSR1, bit);

  // Clear any pending and enable interrupt
  reg_wr(EXTI_RPR1, 1 << bit);
  reg_wr(EXTI_FPR1, 1 << bit);
  reg_set_bit(EXTI_IMR1, bit);

  irq_enable(20 + bit, level);
  irq_permit(q);
}


static void __attribute__((noinline))
gpio_irq(int line)
{
  uint32_t mask = 1 << line;
  reg_wr(EXTI_RPR1, mask);
  reg_wr(EXTI_FPR1, mask);
  gpio_irqs[line].cb(gpio_irqs[line].arg);
}

void irq_20(void) { gpio_irq(0); }
void irq_21(void) { gpio_irq(1); }
void irq_22(void) { gpio_irq(2); }
void irq_23(void) { gpio_irq(3); }
void irq_24(void) { gpio_irq(4); }
void irq_25(void) { gpio_irq(5); }
void irq_26(void) { gpio_irq(6); }
void irq_27(void) { gpio_irq(7); }
void irq_28(void) { gpio_irq(8); }
void irq_29(void) { gpio_irq(9); }
void irq_30(void) { gpio_irq(10); }
void irq_31(void) { gpio_irq(11); }
void irq_32(void) { gpio_irq(12); }
void irq_33(void) { gpio_irq(13); }
void irq_34(void) { gpio_irq(14); }
void irq_35(void) { gpio_irq(15); }
