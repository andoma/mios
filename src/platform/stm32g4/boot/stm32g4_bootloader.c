#include <stddef.h>
#include <sys/param.h>

#include "stm32g4_reg.h"
#include "stm32g4_clk.h"
#include "stm32g4_wdog.h"

#define BLOCKSIZE 32

#ifndef GPIO_MODER_PA
#define GPIO_MODER_PA 0
#endif

#ifndef GPIO_MODER_PB
#define GPIO_MODER_PB 0
#endif

#ifndef GPIO_MODER_PC
#define GPIO_MODER_PC 0
#endif

#ifndef GPIO_MODER_PD
#define GPIO_MODER_PD 0
#endif

//======================================================================
// USART
//======================================================================

#define USART1_BASE 0x40013800
#define USART2_BASE 0x40004400
#define USART3_BASE 0x40004800


#define USART_CR1  0x00
#define USART_CR2  0x04
#define USART_CR3  0x08
#define USART_BRR  0x0c
#define USART_SR   0x1c
#define USART_ICR  0x20
#define USART_RDR  0x24
#define USART_TDR  0x28

#define USART_CR1_UE     (1 << 0)
#define USART_CR1_RE     (1 << 2)
#define USART_CR1_TE     (1 << 3)
#define USART_CR1_RXNEIE (1 << 5)
#define USART_CR1_TCIE   (1 << 6)
#define USART_CR1_TXEIE  (1 << 7)

#define USART_SR_BUSY    (1 << 16)

__attribute__((section("bltext"),noinline))
static void
console_print(char c)
{
  while(!(reg_rd(USART_CONSOLE + USART_SR) & (1 << 7))) {}
  reg_wr(USART_CONSOLE + USART_TDR, c);
}

__attribute__((section("bltext"),noinline,unused))
static void
console_print_nibble(uint8_t n)
{
  console_print(n < 10 ? n + '0' : n + 'a' - 10);
}

__attribute__((section("bltext"),noinline,unused))
static void
console_print_u8(uint8_t n)
{
  for(int i = 4; i >= 0; i -= 4) {
    console_print_nibble((n >> i) & 0xf);
  }
}

__attribute__((section("bltext"),noinline,unused))
static void
console_print_u32(uint32_t n)
{
  for(int i = 28; i >= 0; i -= 4) {
    console_print_nibble((n >> i) & 0xf);
  }
}

__attribute__((section("bltext"),noinline))
static void
console_print_string(const char *str)
{
  while(*str) {
    console_print(*str);
    str++;
  }
}


//======================================================================
// GPIO
//======================================================================

#define GPIO_PORT_ADDR(x) (0x48000000 + ((x) * 0x400))

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
// SPI
//======================================================================

#define SPI1_BASE  0x40013000

#define SPI_CR1    0x00
#define SPI_CR2    0x04
#define SPI_SR     0x08
#define SPI_DR     0x0c

//======================================================================
// REGISTER INIT
//======================================================================

__attribute__((section("bldata")))
static const uint32_t reginit[] = {

  // Clocks
  RCC_AHB1ENR,     (1 << 12) | (1 << 8),  // CRC and FLASH
  RCC_APB2ENR,     (1 << 14) | (1 << 12), // CLK_USART1 | CLK_SPI1
  RCC_APB1ENR1,    (1 << 17) | (1 << 18), // CLK_USART2 | CLK_USART3
  RCC_AHB2ENR,     0xf,                   // CLK_GPIO{A,B,C,D}

  // -----------------------------------------------------
  // Console
  // -----------------------------------------------------

  USART_CONSOLE + USART_BRR, 139, // 115200 BAUD

  USART_CONSOLE + USART_CR1, (USART_CR1_UE | USART_CR1_TE), // Enable UART TX

  // -----------------------------------------------------
  // GPIO PORT A
  // -----------------------------------------------------

  GPIO_PUPDR(PA),
#ifdef USART2_PA2_PA3
  GPIO_BITPAIR(2, 1) | GPIO_BITPAIR(3, 1) |
#endif
  0x24000000,  // SWDIO SWCLK

  GPIO_AFRL(PA),
#ifdef USART2_PA2_PA3
  GPIO_BITQUAD(2, 7) | GPIO_BITQUAD(3, 7) |
#endif
  0,

  GPIO_MODER(PA),
#ifdef USART2_PA2_PA3
  GPIO_BITPAIR(2, 2) | GPIO_BITPAIR(3, 2) |
#endif
  GPIO_MODER_PA |
  0xeb000000, // SWDIO SWCLK

  // -----------------------------------------------------
  // GPIO PORT B
  // -----------------------------------------------------

  GPIO_PUPDR(PB),
#ifdef USART1_PB6_PB7
  GPIO_BITPAIR(6, 1) | GPIO_BITPAIR(7, 1) |
#endif
#ifdef USART3_PB8_PB9
  GPIO_BITPAIR(8, 1) | GPIO_BITPAIR(9, 1) |
#endif
  0,

  GPIO_AFRL(PB),
#ifdef SPI1_PB3_PB4_PB5
  GPIO_BITQUAD(3, 5) | GPIO_BITQUAD(4, 5) | GPIO_BITQUAD(5, 5) |
#endif
  0,

  GPIO_AFRH(PB),
#ifdef USART3_PB8_PB9
  GPIO_BITQUAD(0, 7) | GPIO_BITQUAD(1, 7) |
#endif
  0,

  GPIO_MODER(PB),
#ifdef SPI1_PB3_PB4_PB5
  GPIO_BITPAIR(3, 2) | GPIO_BITPAIR(4, 2) | GPIO_BITPAIR(5, 2) |
#endif
#ifdef USART1_PB6_PB7
  GPIO_BITPAIR(6, 2) | GPIO_BITPAIR(7, 2) |
#endif
#ifdef USART3_PB8_PB9
  GPIO_BITPAIR(8, 2) | GPIO_BITPAIR(9, 2) |
#endif
  GPIO_MODER_PB,

  // -----------------------------------------------------
  // GPIO PORT C
  // -----------------------------------------------------

  GPIO_MODER(PC),
#ifdef USART3_PC10_PC11
  GPIO_BITPAIR(10, 2) | GPIO_BITPAIR(11, 2) |
#endif
  GPIO_MODER_PC,

  GPIO_AFRH(PC),
#ifdef USART3_PC10_PC11
  GPIO_BITQUAD(2, 7) | GPIO_BITQUAD(3, 7) |
#endif
  0,

  // -----------------------------------------------------
  // GPIO PORT D
  // -----------------------------------------------------

  GPIO_MODER(PD),  GPIO_MODER_PD,

  // -----------------------------------------------------
  // SPI
  // -----------------------------------------------------

  SPIFLASH + SPI_CR1, (1 << 9) | (1 << 8) | (1 << 6) | (1 << 2) | (1 << 3),
  SPIFLASH + SPI_CR2, (1 << 12),

  // -----------------------------------------------------
  // Watchdog
  // -----------------------------------------------------

  IWDG_KR, 0x5555,
  IWDG_RLR, 128 * 30, // 30 seconds timeout (same as mios)
  IWDG_PR, 6,  // Prescaler (/256) for 32768 -> 128
  IWDG_KR, 0xAAAA,
  IWDG_KR, 0xCCCC
};


//======================================================================
// CRC-32
//======================================================================
#define CRC_BASE 0x40023000

#define CRC_DR   0x00
#define CRC_IDR  0x04
#define CRC_CR   0x08
#define CRC_INIT 0x10
#define CRC_POL  0x14


__attribute__((always_inline))
static inline uint32_t
bitrev(uint32_t b)
{
  uint32_t mask = 0b11111111111111110000000000000000;
  b = (b & mask) >> 16 | (b & ~mask) << 16;
  mask = 0b11111111000000001111111100000000;
  b = (b & mask) >> 8 | (b & ~mask) << 8;
  mask = 0b11110000111100001111000011110000;
  b = (b & mask) >> 4 | (b & ~mask) << 4;
  mask = 0b11001100110011001100110011001100;
  b = (b & mask) >> 2 | (b & ~mask) << 2;
  mask = 0b10101010101010101010101010101010;
  b = (b & mask) >> 1 | (b & ~mask) << 1;
  return b;
}


__attribute__((section("bltext"),noinline, unused))
static uint32_t
bl_crc32(uint32_t in, const void *data, size_t len)
{
  in = in ? bitrev(~in) : -1;

  reg_wr(CRC_BASE + CRC_INIT, in);
  reg_wr(CRC_BASE + CRC_CR, 0xa1);

  const uint8_t *d8 = data;
  size_t i;
  for(i = 0; i + 4 <= len; i += 4)
    reg_wr(CRC_BASE + CRC_DR, __builtin_bswap32(*(uint32_t *)&d8[i]));

  for(; i + 2 <= len; i += 2)
    reg_wr16(CRC_BASE + CRC_DR, __builtin_bswap16(*(uint16_t *)&d8[i]));

  for(; i < len; i++)
    reg_wr8(CRC_BASE + CRC_DR, d8[i]);

  return ~reg_rd(CRC_BASE + CRC_DR);
}

//======================================================================
// Flash
//======================================================================

#define FLASH_BASE 0x40022000

#define FLASH_KEYR    (FLASH_BASE + 0x08)
#define FLASH_SR      (FLASH_BASE + 0x10)
#define FLASH_CR      (FLASH_BASE + 0x14)

static void __attribute__((section("bltext"),noinline,unused))
flash_unlock(void)
{
  reg_wr(FLASH_KEYR, 0x45670123);
  reg_wr(FLASH_KEYR, 0xCDEF89AB);
}

static void __attribute__((section("bltext"),noinline,unused))
flash_lock(void)
{
  reg_wr(FLASH_CR, 0x80000000);
}

static void __attribute__((section("bltext"),noinline,unused))
flash_erase_page(int page)
{
  reg_wr(FLASH_CR, 0x2 | (page << 3));
  reg_set_bit(FLASH_CR, 16);
  while(reg_rd(FLASH_SR) & (1 << 16)) {}
}

static void __attribute__((section("bltext"),noinline,unused))
flash_write(uint32_t offset, const uint8_t *s)
{
  volatile uint32_t *dst = (void *)0x8000000 + offset;
  reg_wr(FLASH_CR, 0x1);
  for(size_t i = 0; i < BLOCKSIZE; i+= 8) {
    const uint32_t w0 = s[0] | (s[1] << 8) | (s[2] << 16) | (s[3] << 24);
    const uint32_t w1 = s[4] | (s[5] << 8) | (s[6] << 16) | (s[7] << 24);
    dst[0] = w0;
    dst[1] = w1;
    s += 8;
    dst += 2;
    while(reg_rd(FLASH_SR) & (1 << 16)) {}
  }
}


//======================================================================
// SPI
//======================================================================

__attribute__((section("bltext"),noinline))
static uint8_t
spi_txrx(uint8_t out)
{
  reg_wr8(SPIFLASH + SPI_DR, out);
  while(1) {
    uint32_t sr = reg_rd(SPIFLASH + SPI_SR);
    if((sr & 3) == 3)
      break;
  }
  return reg_rd(SPIFLASH + SPI_DR);
}


__attribute__((section("bltext"),noinline))
static void
spiflash_enable(void)
{
  reg_wr(GPIO_BSRR(SPIFLASHCS_PORT), (1 << (16 + SPIFLASHCS_BIT)));
}

__attribute__((section("bltext"),noinline))
static void
spiflash_disable(void)
{
  reg_wr(GPIO_BSRR(SPIFLASHCS_PORT), (1 << SPIFLASHCS_BIT));
}


__attribute__((section("bltext"),noinline))
static void
delay(void)
{
    for(int i = 0; i < 10000; i++) {
      asm volatile("nop");
    }
}

__attribute__((section("bltext"),noinline))
static uint8_t
spiflash_wakeup(void)
{
  delay();
  spiflash_enable();
  delay();
  uint8_t id;
  for(int i = 0; i < 5; i++) {
    id = spi_txrx(0xab);
  }
  spiflash_disable();
  return id;
}


__attribute__((section("bltext"),noinline,unused))
static void
spiflash_read(void *ptr, uint32_t addr, size_t len)
{
  uint8_t *u8 = ptr;
  spiflash_enable();
  spi_txrx(0x3);
  spi_txrx(addr >> 16);
  spi_txrx(addr >> 8);
  spi_txrx(addr);
  for(size_t i = 0; i < len; i++) {
    u8[i] = spi_txrx(0);
  }
  spiflash_disable();
}

__attribute__((section("bltext"),noinline))
static void
spiflash_wait_ready(void)
{
  while(1) {
    delay();
    spiflash_enable();
    spi_txrx(5);
    uint8_t status = spi_txrx(0);
    spiflash_disable();
    if((status & 1) == 0)
      break;
  }
}


__attribute__((section("bltext"),noinline,unused))
static void
spiflash_erase0(void)
{
  spiflash_wait_ready();

  spiflash_enable();
  spi_txrx(0x6); // Write-enable
  spiflash_disable();

  delay();

  spiflash_enable();
  spi_txrx(0x20);
  spi_txrx(0);
  spi_txrx(0);
  spi_txrx(0);
  spiflash_disable();

  spiflash_wait_ready();
}


//======================================================================
// Main
//======================================================================

typedef struct {
  uint32_t magic;
  uint32_t size;
  uint32_t image_crc;
  uint32_t header_crc;
} otahdr_t;

__attribute__((section("bldata")))
static const char welcomestr[] = "\nSTM32 SPI Bootloader, flashid:0x";

__attribute__((section("bldata")))
static const char valid_image[] = "Valid image found, flashing: ";

__attribute__((section("bldata")))
static const char successful[] = "Successful";

__attribute__((section("bldata")))
static const char hfstr[] = "\nHard fault\n";


static void  __attribute__((section("bltext"),noinline))
reboot(void)
{
  static volatile uint32_t *const AIRCR  = (volatile uint32_t *)0xe000ed0c;
  *AIRCR = 0x05fa0004;
  while(1) {}
}

extern uint32_t vectors[];

void __attribute__((section("bltext"),noinline,noreturn)) bl_start(void)
{
  for(size_t i = 0; i < sizeof(reginit) / sizeof(reginit[0]); i += 2) {
    reg_wr(reginit[i + 0], reginit[i + 1]);
  }

  spiflash_disable();

  console_print_string(welcomestr);

  uint8_t code = spiflash_wakeup();
  console_print_u8(code);
  console_print(' ');

  otahdr_t hdr;
  spiflash_read(&hdr, 0, 16);
  uint32_t crc = ~bl_crc32(0, &hdr, sizeof(hdr));
  if(!crc && hdr.magic == 0x3141544f) {
    // Upgrade header is valid

    uint32_t buffer[BLOCKSIZE / sizeof(uint32_t)];

    crc = 0;
    for(size_t i = 0; i < hdr.size; i += BLOCKSIZE) {
      size_t chunk = MIN(BLOCKSIZE, hdr.size - i);
      spiflash_read(buffer, 2048 + i, chunk);
      crc = bl_crc32(crc, buffer, chunk);
    }
    crc = ~crc;
    if(crc == hdr.image_crc) {
      console_print_string(valid_image);
      flash_unlock();

      for(size_t i = 0; i < hdr.size; i += BLOCKSIZE) {
        spiflash_read(buffer, 2048 + i, BLOCKSIZE);

        int page = (i >> 11) + 1;
        if((i & 2047) == 0) {
          console_print('.');
          flash_erase_page(page);
        }
        flash_write(2048 + i, (const void *)buffer);
      }

      flash_lock();
      crc = ~bl_crc32(0, (void*)(intptr_t)2048, hdr.size);
      if(crc == hdr.image_crc) {
        console_print_string(successful);
        spiflash_erase0();
      } else {
        reboot();
      }
    }
  }
  console_print('\n');

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
  console_print_string(hfstr);
  while(1) {}
}
