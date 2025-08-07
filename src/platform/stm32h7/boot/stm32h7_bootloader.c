#include <stdint.h>
#include <stddef.h>
#include <sys/param.h>

#include "stm32h7_clk.h"

#define USART_CONSOLE 0x40011000
#define SPIFLASH_CS GPIO_PA(4)

#define BLOCKSIZE 32

//======================================================================
// GPIO
//======================================================================

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


#define GPIO(PORT, BIT) (((PORT) << 4) | (BIT))

#define GPIO_PA(x)  GPIO(0, x)
#define GPIO_PB(x)  GPIO(1, x)
#define GPIO_PC(x)  GPIO(2, x)
#define GPIO_PD(x)  GPIO(3, x)
#define GPIO_PE(x)  GPIO(4, x)
#define GPIO_PF(x)  GPIO(5, x)
#define GPIO_PG(x)  GPIO(6, x)
#define GPIO_PH(x)  GPIO(7, x)
#define GPIO_PI(x)  GPIO(8, x)
#define GPIO_PJ(x)  GPIO(9, x)
#define GPIO_PK(x)  GPIO(10, x)


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

typedef enum {
  GPIO_FALLING_EDGE    = 0x1,
  GPIO_RISING_EDGE     = 0x2,
  GPIO_BOTH_EDGES      = 0x3,
} gpio_edge_t;

typedef uint8_t gpio_t;


static void __attribute__((section("bltext"),noinline,unused))
gpio_conf_input(gpio_t gpio, gpio_pull_t pull)
{
  const int port = gpio >> 4;
  const int bit = gpio & 0xf;

  clk_enable(CLK_GPIO(port));
  reg_set_bits(GPIO_MODER(port), bit * 2, 2, 0);
  reg_set_bits(GPIO_PUPDR(port), bit * 2, 2, pull);
}



static void __attribute__((section("bltext"),noinline,unused))
gpio_conf_output(gpio_t gpio,
                 gpio_output_type_t type,
                 gpio_output_speed_t speed,
                 gpio_pull_t pull)
{
  const int port = gpio >> 4;
  const int bit = gpio & 0xf;

  clk_enable(CLK_GPIO(port));
  reg_set_bits(GPIO_OTYPER(port),  bit, 1, type);
  reg_set_bits(GPIO_OSPEEDR(port), bit * 2, 2, speed);
  reg_set_bits(GPIO_PUPDR(port), bit * 2, 2, pull);
  reg_set_bits(GPIO_MODER(port), bit * 2, 2, 1);
}


static void __attribute__((section("bltext"),noinline,unused))
gpio_conf_af(gpio_t gpio, int af, gpio_output_type_t type,
             gpio_output_speed_t speed, gpio_pull_t pull)
{
  const int port = gpio >> 4;
  const int bit = gpio & 0xf;

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
}



static void __attribute__((section("bltext"),noinline,unused))
gpio_set_output(gpio_t gpio, int on)
{
  const int port = gpio >> 4;
  const int bit = gpio & 0xf;

  reg_set(GPIO_BSRR(port), 1 << (bit + !on * 16));
}

//======================================================================
// CONSOLE
//======================================================================


#define USART_CR1_UE     (1 << 0)
#define USART_CR1_UESM   (1 << 1)
#define USART_CR1_RE     (1 << 2)
#define USART_CR1_TE     (1 << 3)
#define USART_CR1_RXNEIE (1 << 5)
#define USART_CR1_TCIE   (1 << 6)
#define USART_CR1_TXEIE  (1 << 7)

#define USART_CR1  0x00
#define USART_CR2  0x04
#define USART_CR3  0x08
#define USART_BRR  0x0c
#define USART_SR   0x1c
#define USART_ICR  0x20
#define USART_RDR  0x24
#define USART_TDR  0x28

__attribute__((section("bltext"),noinline))
static void
console_print(char c)
{
#ifdef USART_CONSOLE
  while(!(reg_rd(USART_CONSOLE + USART_SR) & (1 << 7))) {}
  reg_wr(USART_CONSOLE + USART_TDR, c);
#endif
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
// CRC-32
//======================================================================
#define CRC_BASE 0x58024C00

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
// SPI
//======================================================================


#define SPI_CR1    0x00
#define SPI_CR2    0x04
#define SPI_CFG1   0x08
#define SPI_CFG2   0x0c
#define SPI_IER    0x10
#define SPI_SR     0x14
#define SPI_IFCR   0x18
#define SPI_TXDR   0x20
#define SPI_RXDR   0x30

#define SPI1_BASE 0x40013000
#define SPI2_BASE 0x40003800
#define SPI3_BASE 0x40003c00

#define SPIBUS SPI1_BASE


__attribute__((section("bltext"),noinline))
static void
spi_init(void)
{
  uint32_t cfg2 =
    (1 << 28)             | // (internal) SS is active high
    (1 << 26)             | // Software management of SS
    (1 << 22)             | // Master mode
    (1 << 31)             | // Take control over IO even when disabled
    0;

  uint32_t cfg1 =
    (0b100 << 28)         | // Baudrate divisor
    (0b111 << 0)          | // DSIZE (8 bit)
    0;

  reg_wr(SPIBUS + SPI_CFG1, cfg1);
  reg_wr(SPIBUS + SPI_CFG2, cfg2);
  reg_wr(SPIBUS + SPI_CR2, 0);

  reg_wr(SPIBUS + SPI_CR1,
         (1 << 0) | // Enable
         0);

  reg_wr(SPIBUS + SPI_CR1,
         (1 << 9) | // master transfer enable
         (1 << 0) | // Enable
         0);

}


__attribute__((section("bltext"),noinline))
static uint8_t
spi_txrx(uint8_t out)
{
  reg_wr8(SPIBUS + SPI_TXDR, out);
  while(((reg_rd(SPIBUS + SPI_SR) & 1)) == 0) {
  }
  return reg_rd8(SPIBUS + SPI_RXDR);
}


__attribute__((section("bltext"),noinline))
static void
spiflash_enable(void)
{
  gpio_set_output(SPIFLASH_CS, 0);
}

__attribute__((section("bltext"),noinline))
static void
spiflash_disable(void)
{
  gpio_set_output(SPIFLASH_CS, 1);
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
// FLASH
//======================================================================

#define FLASH_BASE 0x52002000

#define FLASH_KEYR (FLASH_BASE + 0x004)
#define FLASH_CR   (FLASH_BASE + 0x00c)
#define FLASH_SR   (FLASH_BASE + 0x010)


static void __attribute__((section("bltext"),noinline,unused))
flash_unlock(void)
{
  reg_wr(FLASH_KEYR, 0x45670123);
  reg_wr(FLASH_KEYR, 0xCDEF89AB);
}

static void __attribute__((section("bltext"),noinline,unused))
flash_lock(void)
{
  reg_wr(FLASH_CR, 1);
}

static void __attribute__((section("bltext"),noinline,unused))
flash_erase_sector(int sector)
{
  reg_wr(FLASH_CR,
         (1 << 2) |     // SER
         (sector << 8));
  reg_set_bit(FLASH_CR, 7);
  while(reg_rd(FLASH_SR) & (1 << 2)) {}
}

static void __attribute__((section("bltext"),noinline,unused))
flash_write(uint32_t offset, const uint8_t *s)
{
  volatile uint32_t *dst = (void *)0x8000000 + offset;
  reg_wr(FLASH_CR, 0x2);
  for(size_t i = 0; i < BLOCKSIZE; i+= 8) {
    const uint32_t w0 = s[0] | (s[1] << 8) | (s[2] << 16) | (s[3] << 24);
    const uint32_t w1 = s[4] | (s[5] << 8) | (s[6] << 16) | (s[7] << 24);
    dst[0] = w0;
    dst[1] = w1;
    s += 8;
    dst += 2;
    while(reg_rd(FLASH_SR) & (1 << 2)) {}
  }
}



//======================================================================
// Main
//======================================================================

static void  __attribute__((section("bltext"),noinline,unused))
reboot(void)
{
  static volatile uint32_t *const AIRCR  = (volatile uint32_t *)0xe000ed0c;
  *AIRCR = 0x05fa0004;
  while(1) {}
}


__attribute__((section("bldata")))
static const char welcomestr[] = "\nMIOS STM32H7 SPI Bootloader, flashid:0x";

__attribute__((section("bldata")))
static const char valid_header[] = "\n* Valid header found";

__attribute__((section("bldata")))
static const char valid_image[] = "\n* Valid image found, flashing: ";

__attribute__((section("bldata")))
static const char successful[] = " Successful\n";

__attribute__((section("bldata")))
static const char hfstr[] = "\nHard fault\n";

__attribute__((section("bldata")))
static const char mmfstr[] = "\nMM fault\n";

__attribute__((section("bldata")))
static const char busfstr[] = "\nBus fault\n";

__attribute__((section("bldata")))
static const char usagefstr[] = "\nUsage fault\n";



typedef struct {
  uint32_t magic;
  uint32_t size;
  uint32_t image_crc;
  uint32_t header_crc;
} otahdr_t;

extern uint32_t vectors[];

void __attribute__((section("bltext"),noinline,noreturn)) bl_start(void)
{
  clk_enable(CLK_CRC);

  clk_enable(CLK_USART1);
  gpio_conf_af(GPIO_PA(9), 7, GPIO_PUSH_PULL, GPIO_SPEED_LOW, GPIO_PULL_NONE);

  reg_wr(RCC_D2CCIP1R,
         (0b100 << 12) | // SPI1,2,3 clocked from kernel clock
         0);

  clk_enable(CLK_SPI1);
  gpio_conf_af(GPIO_PA(5), 5, GPIO_PUSH_PULL, GPIO_SPEED_LOW, GPIO_PULL_NONE);
  gpio_conf_af(GPIO_PA(6), 5, GPIO_OPEN_DRAIN, GPIO_SPEED_LOW, GPIO_PULL_UP);
  gpio_conf_af(GPIO_PA(7), 5, GPIO_PUSH_PULL, GPIO_SPEED_LOW, GPIO_PULL_NONE);

  gpio_conf_output(SPIFLASH_CS, GPIO_PUSH_PULL, GPIO_SPEED_LOW, GPIO_PULL_NONE);
  spiflash_disable();

  spi_init();

  reg_wr(USART_CONSOLE + USART_BRR, 556); // 115200
  reg_wr(USART_CONSOLE + USART_CR1, (USART_CR1_UE | USART_CR1_TE));

  console_print_string(welcomestr);
  uint8_t code = spiflash_wakeup();
  console_print_u8(code);

  otahdr_t hdr;
  spiflash_read(&hdr, 0, 16);
  uint32_t crc = ~bl_crc32(0, &hdr, sizeof(hdr));
  if(!crc && hdr.magic == 0x3141544f) {
    // Upgrade header is valid
    console_print_string(valid_header);

    uint32_t buffer[BLOCKSIZE / sizeof(uint32_t)];

    crc = 0;
    for(size_t i = 0; i < hdr.size; i += BLOCKSIZE) {
      size_t chunk = MIN(BLOCKSIZE, hdr.size - i);
      spiflash_read(buffer, 4096 + i, chunk);
      crc = bl_crc32(crc, buffer, chunk);
    }
    crc = ~crc;
    if(crc == hdr.image_crc) {
      console_print_string(valid_image);

      flash_unlock();

      for(size_t i = 0; i < hdr.size; i += BLOCKSIZE) {
        spiflash_read(buffer, 4096 + i, BLOCKSIZE);

        uint32_t flash_addr = i + 0x20000;
        uint32_t sector = flash_addr >> 17;

        if((flash_addr & 0x1ffff) == 0) {
          console_print('.');
          flash_erase_sector(sector);
        }
        flash_write(flash_addr, (const void *)buffer);
      }
      flash_lock();

      crc = ~bl_crc32(0, (void*)(intptr_t)0x8020000, hdr.size);
      if(crc == hdr.image_crc) {
        console_print_string(successful);
        spiflash_erase0();
      } else {
        reboot();
      }
    }
  }

  reset_peripheral(CLK_SPI1);

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

void __attribute__((section("bltext"),noinline))
bl_mm_fault(void)
{
  console_print_string(mmfstr);
  while(1) {}
}

void __attribute__((section("bltext"),noinline))
bl_bus_fault(void)
{
  console_print_string(busfstr);
  while(1) {}
}

void __attribute__((section("bltext"),noinline))
bl_usage_fault(void)
{
  console_print_string(usagefstr);
  while(1) {}
}

