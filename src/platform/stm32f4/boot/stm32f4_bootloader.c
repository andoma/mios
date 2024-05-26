#include <stddef.h>
#include <sys/param.h>

#include "stm32f4_reg.h"
#include "stm32f4_clk.h"

#include "version_git.h"

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
// SPI
//======================================================================

#define SPI1_BASE 0x40013000
#define SPI3_BASE 0x40003c00

#define SPI_CR1    0x00
#define SPI_CR2    0x04
#define SPI_SR     0x08
#define SPI_DR     0x0c

//======================================================================
// REGISTER INIT
//======================================================================

__attribute__((section("bldata")))
static const uint32_t reginit[] = {

  RCC_APB1ENR,
#if USART_CONSOLE == USART3_BASE
  (1 << 18) |
#endif
  0,

  RCC_APB2ENR,
#if SPIFLASH == SPI1_BASE
  (1 << 12) |
#endif
  0,

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

  // -----------------------------------------------------
  // GPIO PORT B
  // -----------------------------------------------------

  GPIO_AFRL(PB),
#ifdef SPI1_PB3_PB4_PB5
  GPIO_BITQUAD(3, 5) | GPIO_BITQUAD(4, 5) | GPIO_BITQUAD(5, 5) |
#endif
  0,

  GPIO_MODER(PB),
#ifdef SPI1_PB3_PB4_PB5
  GPIO_BITPAIR(3, 2) | GPIO_BITPAIR(4, 2) | GPIO_BITPAIR(5, 2) |
#endif
  0,

  // -----------------------------------------------------
  // GPIO PORT C
  // -----------------------------------------------------

  GPIO_MODER(PC),
#ifdef USART3_PC10_PC11
  GPIO_BITPAIR(10, 2) | GPIO_BITPAIR(11, 2) |
#endif
  0,

  GPIO_AFRH(PC),
#ifdef USART3_PC10_PC11
  GPIO_BITQUAD(2, 7) | GPIO_BITQUAD(3, 7) |
#endif
  0,

  // -----------------------------------------------------
  // GPIO PORT D
  // -----------------------------------------------------
  GPIO_MODER(PD),
  GPIO_MODER_PD,

  // -----------------------------------------------------
  // SPI
  // -----------------------------------------------------

  SPIFLASH + SPI_CR1, (1 << 9) | (1 << 8) | (1 << 6) | (1 << 2) | (1 << 3),
  SPIFLASH + SPI_CR2, (1 << 12),
};

// -----------------------------------------------------
// CRC32
// -----------------------------------------------------
__attribute__((section("bldata")))
static const uint32_t bl_crc32table[256] = {
  0xd202ef8d, 0xa505df1b, 0x3c0c8ea1, 0x4b0bbe37, 0xd56f2b94, 0xa2681b02,
  0x3b614ab8, 0x4c667a2e, 0xdcd967bf, 0xabde5729, 0x32d70693, 0x45d03605,
  0xdbb4a3a6, 0xacb39330, 0x35bac28a, 0x42bdf21c, 0xcfb5ffe9, 0xb8b2cf7f,
  0x21bb9ec5, 0x56bcae53, 0xc8d83bf0, 0xbfdf0b66, 0x26d65adc, 0x51d16a4a,
  0xc16e77db, 0xb669474d, 0x2f6016f7, 0x58672661, 0xc603b3c2, 0xb1048354,
  0x280dd2ee, 0x5f0ae278, 0xe96ccf45, 0x9e6bffd3, 0x0762ae69, 0x70659eff,
  0xee010b5c, 0x99063bca, 0x000f6a70, 0x77085ae6, 0xe7b74777, 0x90b077e1,
  0x09b9265b, 0x7ebe16cd, 0xe0da836e, 0x97ddb3f8, 0x0ed4e242, 0x79d3d2d4,
  0xf4dbdf21, 0x83dcefb7, 0x1ad5be0d, 0x6dd28e9b, 0xf3b61b38, 0x84b12bae,
  0x1db87a14, 0x6abf4a82, 0xfa005713, 0x8d076785, 0x140e363f, 0x630906a9,
  0xfd6d930a, 0x8a6aa39c, 0x1363f226, 0x6464c2b0, 0xa4deae1d, 0xd3d99e8b,
  0x4ad0cf31, 0x3dd7ffa7, 0xa3b36a04, 0xd4b45a92, 0x4dbd0b28, 0x3aba3bbe,
  0xaa05262f, 0xdd0216b9, 0x440b4703, 0x330c7795, 0xad68e236, 0xda6fd2a0,
  0x4366831a, 0x3461b38c, 0xb969be79, 0xce6e8eef, 0x5767df55, 0x2060efc3,
  0xbe047a60, 0xc9034af6, 0x500a1b4c, 0x270d2bda, 0xb7b2364b, 0xc0b506dd,
  0x59bc5767, 0x2ebb67f1, 0xb0dff252, 0xc7d8c2c4, 0x5ed1937e, 0x29d6a3e8,
  0x9fb08ed5, 0xe8b7be43, 0x71beeff9, 0x06b9df6f, 0x98dd4acc, 0xefda7a5a,
  0x76d32be0, 0x01d41b76, 0x916b06e7, 0xe66c3671, 0x7f6567cb, 0x0862575d,
  0x9606c2fe, 0xe101f268, 0x7808a3d2, 0x0f0f9344, 0x82079eb1, 0xf500ae27,
  0x6c09ff9d, 0x1b0ecf0b, 0x856a5aa8, 0xf26d6a3e, 0x6b643b84, 0x1c630b12,
  0x8cdc1683, 0xfbdb2615, 0x62d277af, 0x15d54739, 0x8bb1d29a, 0xfcb6e20c,
  0x65bfb3b6, 0x12b88320, 0x3fba6cad, 0x48bd5c3b, 0xd1b40d81, 0xa6b33d17,
  0x38d7a8b4, 0x4fd09822, 0xd6d9c998, 0xa1def90e, 0x3161e49f, 0x4666d409,
  0xdf6f85b3, 0xa868b525, 0x360c2086, 0x410b1010, 0xd80241aa, 0xaf05713c,
  0x220d7cc9, 0x550a4c5f, 0xcc031de5, 0xbb042d73, 0x2560b8d0, 0x52678846,
  0xcb6ed9fc, 0xbc69e96a, 0x2cd6f4fb, 0x5bd1c46d, 0xc2d895d7, 0xb5dfa541,
  0x2bbb30e2, 0x5cbc0074, 0xc5b551ce, 0xb2b26158, 0x04d44c65, 0x73d37cf3,
  0xeada2d49, 0x9ddd1ddf, 0x03b9887c, 0x74beb8ea, 0xedb7e950, 0x9ab0d9c6,
  0x0a0fc457, 0x7d08f4c1, 0xe401a57b, 0x930695ed, 0x0d62004e, 0x7a6530d8,
  0xe36c6162, 0x946b51f4, 0x19635c01, 0x6e646c97, 0xf76d3d2d, 0x806a0dbb,
  0x1e0e9818, 0x6909a88e, 0xf000f934, 0x8707c9a2, 0x17b8d433, 0x60bfe4a5,
  0xf9b6b51f, 0x8eb18589, 0x10d5102a, 0x67d220bc, 0xfedb7106, 0x89dc4190,
  0x49662d3d, 0x3e611dab, 0xa7684c11, 0xd06f7c87, 0x4e0be924, 0x390cd9b2,
  0xa0058808, 0xd702b89e, 0x47bda50f, 0x30ba9599, 0xa9b3c423, 0xdeb4f4b5,
  0x40d06116, 0x37d75180, 0xaede003a, 0xd9d930ac, 0x54d13d59, 0x23d60dcf,
  0xbadf5c75, 0xcdd86ce3, 0x53bcf940, 0x24bbc9d6, 0xbdb2986c, 0xcab5a8fa,
  0x5a0ab56b, 0x2d0d85fd, 0xb404d447, 0xc303e4d1, 0x5d677172, 0x2a6041e4,
  0xb369105e, 0xc46e20c8, 0x72080df5, 0x050f3d63, 0x9c066cd9, 0xeb015c4f,
  0x7565c9ec, 0x0262f97a, 0x9b6ba8c0, 0xec6c9856, 0x7cd385c7, 0x0bd4b551,
  0x92dde4eb, 0xe5dad47d, 0x7bbe41de, 0x0cb97148, 0x95b020f2, 0xe2b71064,
  0x6fbf1d91, 0x18b82d07, 0x81b17cbd, 0xf6b64c2b, 0x68d2d988, 0x1fd5e91e,
  0x86dcb8a4, 0xf1db8832, 0x616495a3, 0x1663a535, 0x8f6af48f, 0xf86dc419,
  0x660951ba, 0x110e612c, 0x88073096, 0xff000000
};

__attribute__((section("bltext"),noinline,unused))
static uint32_t
bl_crc32(uint32_t crc, const void *data, size_t n_bytes)
{
  for(size_t i = 0; i < n_bytes; ++i)
    crc = bl_crc32table[(uint8_t)crc ^ ((uint8_t*)data)[i]] ^ crc >> 8;

  return crc;
}


//======================================================================
// Flash
//======================================================================

#define FLASH_BASE    0x40023c00

#define FLASH_KEYR    (FLASH_BASE + 0x04)
#define FLASH_SR      (FLASH_BASE + 0x0c)
#define FLASH_CR      (FLASH_BASE + 0x10)

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
flash_erase_sector(int sector)
{
  reg_wr(FLASH_CR, 0x2 | (sector << 3));
  reg_set_bit(FLASH_CR, 16);
  while(reg_rd(FLASH_SR) & (1 << 16)) {
    //    reg_wr(IWDG_KR, 0xAAAA);
  }
}

static void
flash_write(uint32_t addr, const void *src)
{
  while(reg_rd(FLASH_SR) & (1 << 16)) {
  }

  reg_wr(FLASH_CR, 0x1 | (0 << 8));

  const uint8_t *u32 = src;
  volatile uint8_t *dst = (uint8_t *)(0x8000000 + addr);
  for(size_t i = 0; i < BLOCKSIZE; i++) {
    dst[i] = u32[i];
  }
  while(reg_rd(FLASH_SR) & (1 << 16)) {
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

__attribute__((section("bltext"),noinline,unused))
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

extern uint32_t vectors[];

__attribute__((section("bldata")))
static const char welcomestr[] = "\nSTM32 SPI Bootloader, flashid:0x";

__attribute__((section("bldata")))
static const char valid_image[] = "Valid image found, flashing: ";

__attribute__((section("bldata")))
static const char successful[] = "Successful";

static void  __attribute__((section("bltext"),noinline))
reboot(void)
{
  static volatile uint32_t *const AIRCR  = (volatile uint32_t *)0xe000ed0c;
  *AIRCR = 0x05fa0004;
  while(1) {}
}


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
      spiflash_read(buffer, 16384 + i, chunk);
      crc = bl_crc32(crc, buffer, chunk);
    }
    crc = ~crc;
    if(crc == hdr.image_crc) {
      console_print_string(valid_image);

      flash_unlock();

      for(size_t i = 0; i < hdr.size; i += BLOCKSIZE) {
        uint32_t addr = 16384 + i;
        spiflash_read(buffer, addr, BLOCKSIZE);
        int sector = -1;
        if(addr < 65536 && (addr & 16383) == 0) {
          sector = addr >> 14;
        } else if(addr == 65536) {
          sector = 4;
        } else if((addr & 131071) == 0) {
          sector = 4 + (addr >> 17);
        }

        if(sector != -1) {
          console_print('.');
          flash_erase_sector(sector);
        }

        flash_write(addr, (const void *)buffer);
      }

      flash_lock();
      crc = ~bl_crc32(0, (void*)(intptr_t)16384, hdr.size);
      if(crc == hdr.image_crc) {
        console_print_string(successful);
        spiflash_erase0();
      } else {
        while(1) {
          asm volatile("nop");
        }
        reboot();
      }
    }
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
