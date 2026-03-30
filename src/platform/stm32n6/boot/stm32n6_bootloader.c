#include <stdint.h>
#include <stddef.h>

#include "stm32n6_reg.h"
#include "stm32n6_clk.h"

// =====================================================================
// Section attributes
// =====================================================================

#define BL __attribute__((section("bltext"), noinline))
#define BL_DATA __attribute__((section("bldata")))

#define BL_STR(name, val) \
  static const char name[] BL_DATA = val

// =====================================================================
// GPIO (same API as MIOS but self-contained in bootloader)
// =====================================================================

#define GPIO_PORT_ADDR(x) (0x56020000 + ((x) * 0x400))
#define GPIO_MODER(x)     (GPIO_PORT_ADDR(x) + 0x00)
#define GPIO_OTYPER(x)    (GPIO_PORT_ADDR(x) + 0x04)
#define GPIO_OSPEEDR(x)   (GPIO_PORT_ADDR(x) + 0x08)
#define GPIO_PUPDR(x)     (GPIO_PORT_ADDR(x) + 0x0c)
#define GPIO_BSRR(x)      (GPIO_PORT_ADDR(x) + 0x18)
#define GPIO_AFRL(x)      (GPIO_PORT_ADDR(x) + 0x20)
#define GPIO_AFRH(x)      (GPIO_PORT_ADDR(x) + 0x24)

#define GPIO(PORT, BIT) (((PORT) << 4) | (BIT))

#define GPIO_PA(x)  GPIO(0, x)
#define GPIO_PB(x)  GPIO(1, x)
#define GPIO_PC(x)  GPIO(2, x)
#define GPIO_PD(x)  GPIO(3, x)
#define GPIO_PE(x)  GPIO(4, x)
#define GPIO_PF(x)  GPIO(5, x)
#define GPIO_PG(x)  GPIO(6, x)
#define GPIO_PH(x)  GPIO(7, x)
#define GPIO_PN(x)  GPIO(13, x)

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

typedef uint8_t gpio_t;


static void BL
gpio_conf_output(gpio_t gpio, gpio_output_type_t type,
                 gpio_output_speed_t speed, gpio_pull_t pull)
{
  const int port = gpio >> 4;
  const int bit = gpio & 0xf;

  reg_set_bit(RCC_AHB4ENR, port);
  reg_set_bits(GPIO_OTYPER(port),  bit, 1, type);
  reg_set_bits(GPIO_OSPEEDR(port), bit * 2, 2, speed);
  reg_set_bits(GPIO_PUPDR(port), bit * 2, 2, pull);
  reg_set_bits(GPIO_MODER(port), bit * 2, 2, 1);
}


static void BL
gpio_conf_af(gpio_t gpio, int af, gpio_output_type_t type,
             gpio_output_speed_t speed, gpio_pull_t pull)
{
  const int port = gpio >> 4;
  const int bit = gpio & 0xf;

  reg_set_bit(RCC_AHB4ENR, port);
  reg_set_bits(GPIO_OTYPER(port), bit, 1, type);
  reg_set_bits(GPIO_OSPEEDR(port), bit * 2, 2, speed);

  if(bit < 8)
    reg_set_bits(GPIO_AFRL(port), bit * 4, 4, af);
  else
    reg_set_bits(GPIO_AFRH(port), (bit - 8) * 4, 4, af);

  reg_set_bits(GPIO_PUPDR(port), bit * 2, 2, pull);
  reg_set_bits(GPIO_MODER(port), bit * 2, 2, 2);
}


static void BL
gpio_set_output(gpio_t gpio, int on)
{
  const int port = gpio >> 4;
  const int bit = gpio & 0xf;
  reg_wr(GPIO_BSRR(port), 1 << (bit + !on * 16));
}

// =====================================================================
// USART
// Boot ROM leaves clocks in nominal scenario (default fuses):
//   CPU=400MHz, AHB=150MHz, APB=150MHz
// =====================================================================

#define USART1_BASE  0x52001000
#define USART_CR1    0x00
#define USART_BRR    0x0c
#define USART_SR     0x1c
#define USART_TDR    0x28

static void BL
bl_uart_init(int apb_freq)
{
  reg_set_bit(RCC_APB2ENR, 4); // USART1 clock

  gpio_conf_af(GPIO_PE(5), 7, GPIO_PUSH_PULL, GPIO_SPEED_LOW, GPIO_PULL_NONE);
  gpio_conf_af(GPIO_PE(6), 7, GPIO_PUSH_PULL, GPIO_SPEED_LOW, GPIO_PULL_NONE);

  reg_wr(USART1_BASE + USART_CR1, 0);
  reg_wr(USART1_BASE + USART_BRR, apb_freq / 115200);
  reg_wr(USART1_BASE + USART_CR1, (1 << 0) | (1 << 2) | (1 << 3));
}

static void BL
bl_putchar(char c)
{
  while(!(reg_rd(USART1_BASE + USART_SR) & (1 << 7))) {}
  reg_wr(USART1_BASE + USART_TDR, c);
}

static void BL
bl_puts(const char *s)
{
  while(*s) {
    if(*s == '\n')
      bl_putchar('\r');
    bl_putchar(*s++);
  }
}

static void __attribute__((unused)) BL
bl_puthex(uint32_t v)
{
  for(int i = 28; i >= 0; i -= 4) {
    int n = (v >> i) & 0xf;
    bl_putchar(n < 10 ? '0' + n : 'a' + n - 10);
  }
}

// =====================================================================
// Simple delay
// =====================================================================

static void BL
bl_delay(int count)
{
  for(volatile int d = 0; d < count; d++) {}
}

// =====================================================================
// Entry point (called from assembly after DTCM/MSP init)
// =====================================================================

void __attribute__((noreturn)) BL
bl_main(void)
{
  // Blink LED to show we're alive
  gpio_conf_output(GPIO_PG(10), GPIO_PUSH_PULL, GPIO_SPEED_LOW, GPIO_PULL_NONE);
  for(int n = 0; n < 6; n++) {
    gpio_set_output(GPIO_PG(10), n & 1);
    bl_delay(200000);
  }

  // Auto-detect clock: if PLL1 is running (nominal), APB2=150MHz, else 32MHz
  int apb2_freq = (reg_rd(RCC_SR) & (1 << 8)) ? 150000000 : 32000000;
  bl_uart_init(apb2_freq);

  BL_STR(msg_banner, "\nMIOS FSBL v1\n");
  bl_puts(msg_banner);

  BL_STR(msg_halt, "No application, halting.\n");
  bl_puts(msg_halt);

  while(1) {}
}

void __attribute__((noreturn)) BL
bl_fault(void)
{
  BL_STR(msg_fault, "FSBL FAULT\n");
  bl_puts(msg_fault);
  while(1) {}
}
