#include <stddef.h>

#include "stm32f4_reg.h"
#include "stm32f4_clk.h"

#include "version_git.h"

// Move these to BOOTLOADER_DEFS file
#define USART_CONSOLE USART1_BASE
#define USART1_PA9_PA10

//======================================================================
// GPIO
//======================================================================

#define GPIO_PORT_ADDR(x) (0x40020000 + ((x) * 0x400))

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


#define GPIO_BITPAIR(idx, mode) ((mode) << (idx * 2))
#define GPIO_BITQUAD(idx, mode) ((mode) << (idx * 4))

#define PA 0
#define PB 1
#define PC 2
#define PD 3

//======================================================================
// USART
//======================================================================

#define USART1_BASE 0x40011000
#define USART2_BASE 0x40004400
#define USART3_BASE 0x40004800
#define USART4_BASE 0x40004c00
#define USART5_BASE 0x40005000
#define USART6_BASE 0x40011400

#define USART_SR    0x00
#define USART_TDR   0x04
#define USART_RDR   0x04
#define USART_BRR   0x08
#define USART_CR1   0x0c
#define USART_CR2   0x10
#define USART_CR3   0x14

#define USART_CR1_UE     (1 << 13)
#define USART_CR1_RE     (1 << 2)
#define USART_CR1_TE     (1 << 3)
#define USART_CR1_RXNEIE (1 << 5)
#define USART_CR1_TCIE   (1 << 6)
#define USART_CR1_TXEIE  (1 << 7)

__attribute__((section("bltext"),noinline))
static void
console_send(char c)
{
  while(!(reg_rd(USART_CONSOLE + USART_SR) & (1 << 7))) {}
  reg_wr(USART_CONSOLE + USART_TDR, c);
}

__attribute__((section("bltext"),noinline,unused))
static void
console_send_nibble(uint8_t n)
{
  console_send(n < 10 ? n + '0' : n + 'a' - 10);
}

#if 0
__attribute__((section("bltext"),noinline,unused))
static void
console_send_u32(uint32_t n)
{
  for(int i = 28; i >= 0; i -= 4) {
    console_send_nibble((n >> i) & 0xf);
  }
}
#endif

__attribute__((section("bltext"),noinline))
static void
console_send_string(const char *str)
{
  while(*str) {
    console_send(*str);
    str++;
  }
}


//======================================================================
// REGISTER INIT
//======================================================================

__attribute__((section("bldata")))
static const uint32_t reginit[] = {

  // Clocks
  //  RCC_AHBENR,      (1 << 12) | (1 << 8),  // CRC and FLASH
  RCC_APB2ENR,      (1 << 4),                // CLK_USART1
  RCC_AHB1ENR,      (1 << 20) | 0xf,         // CLK_GPIO{A,B,C,D}

  // -----------------------------------------------------
  // Console
  // -----------------------------------------------------

  USART_CONSOLE + USART_BRR, 139, // 115200 BAUD

  USART_CONSOLE + USART_CR1, (USART_CR1_UE | USART_CR1_TE), // Enable UART TX

  // -----------------------------------------------------
  // GPIO PORT A
  // -----------------------------------------------------

  GPIO_PUPDR(PA),
#ifdef USART1_PA9_PA10
  GPIO_BITPAIR(9, 1) | GPIO_BITPAIR(10, 1) |
#endif
  0x64000000,  // SWDIO SWCLK

  GPIO_AFRH(PA),
#ifdef USART1_PA9_PA10
  GPIO_BITQUAD(1, 7) | GPIO_BITQUAD(2, 7) |
#endif
  0,

  GPIO_MODER(PA),
#ifdef USART1_PA9_PA10
  GPIO_BITPAIR(9, 2) | GPIO_BITPAIR(10, 2) |
#endif
  0xa8000000, // SWDIO SWCLK
};





extern uint32_t vectors[];

__attribute__((section("bldata")))
static const char welcomestr[] = "\nBootloader ready\n";

void __attribute__((section("bltext"),noinline,noreturn)) bl_start(void)
{
  if(0) {
    for(size_t i = 0; i < sizeof(reginit) / sizeof(reginit[0]); i += 2) {
      reg_wr(reginit[i + 0], reginit[i + 1]);
    }
    console_send_string(welcomestr);

    while(1) {}
  }
  void (*init)(void) __attribute__((noreturn))  = (void *)vectors[1];
  init();
}

void __attribute__((section("bltext"),noinline))
bl_nmi(void)
{
  while(1) {}
}

void __attribute__((section("bltext"),noinline))
bl_hard_fault(void)
{
  while(1) {}
}

void __attribute__((section("bltext"),noinline))
bl_mm_fault(void)
{
  while(1) {}
}

void __attribute__((section("bltext"),noinline))
bl_bus_fault(void)
{
  while(1) {}
}

void __attribute__((section("bltext"),noinline))
bl_usage_fault(void)
{
  while(1) {}
}
