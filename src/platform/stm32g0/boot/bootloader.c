#include <stddef.h>

#include "stm32g0_reg.h"
#include "stm32g0_clk.h"
#include "stm32g0_wdog.h"

#include "version_git.h"

#define BLOCKSIZE 32
#define BLOCKS_PER_SECTOR (2048 / 32)

#ifndef GPIO_MODER_PA
#define GPIO_MODER_PA 0
#endif

#ifndef GPIO_MODER_PB
#define GPIO_MODER_PB 0
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
console_send(char c)
{
  while(!(reg_rd(USART_CONSOLE + USART_SR) & (1 << 7))) {}
  reg_wr(USART_CONSOLE + USART_TDR, c);
}

__attribute__((section("bltext"),noinline))
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

#define RECV_TIMEOUT_FAST 2048
#define RECV_TIMEOUT_SLOW (1 << 19)

__attribute__((section("bltext"),noinline))
static int
mbus_recv_byte(int timeout)
{
  int c = 0;

  while(!(reg_rd(USART_MBUS + USART_SR) & (1 << 5))) {
    c++;
    if(c == timeout)
      return -1;
  }
  return reg_rd(USART_MBUS + USART_RDR);
}


__attribute__((section("bltext"),noinline))
static void
mbus_send_byte(uint8_t c)
{
  reg_wr(USART_MBUS + USART_TDR, c);
  // Wait for byte to transmit
  while(!(reg_rd(USART_MBUS + USART_SR) & (1 << 6))) {}
  // Read back echo
  mbus_recv_byte(256);
}





//======================================================================
// GPIO
//======================================================================

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


#define GPIO_BITPAIR(idx, mode) ((mode) << (idx * 2))
#define GPIO_BITQUAD(idx, mode) ((mode) << (idx * 4))

#define PA 0
#define PB 1
#define PC 2
#define PD 3





//======================================================================
// STATE
//======================================================================

#define OTA_STATE_WAIT_CONNECT  0
#define OTA_STATE_WAIT_INFO_ACK 1
#define OTA_STATE_WAIT_HEADER   2
#define OTA_STATE_WAIT_DATA     3


typedef struct ota_workmem {
  uint16_t flow;
  uint8_t local_addr;
  uint8_t remote_addr;

  uint8_t rx_len;
  uint8_t tx_len;
  uint8_t pad1;
  uint8_t pad2;

  int block;
  int num_blocks;
  int image_crc;

  uint8_t rx_buf[64];
  uint8_t tx_buf[64];

  uint8_t block0[BLOCKSIZE];

} ota_workmem_t;



//======================================================================
// REGISTER INIT
//======================================================================

__attribute__((section("bldata")))
static const uint32_t reginit[] = {

  // Clocks
  RCC_AHBENR,      (1 << 12) | (1 << 8),  // CRC and FLASH
  RCC_APBENR2,     (1 << 14),             // CLK_USART1
  RCC_APBENR1,     (1 << 17) | (1 << 18), // CLK_USART2 | CLK_USART3
  RCC_IOPENR,      0xf,                   // CLK_GPIO{A,B,C,D}

  // -----------------------------------------------------
  // Console
  // -----------------------------------------------------

  USART_CONSOLE + USART_BRR, 139, // 115200 BAUD

  USART_CONSOLE + USART_CR1, (USART_CR1_UE | USART_CR1_TE), // Enable UART TX

  // -----------------------------------------------------
  // MBUS
  // -----------------------------------------------------

  USART_MBUS + USART_BRR, 139, // 115200 BAUD

  // Enable UART TX & RX
  USART_MBUS + USART_CR1, 0,
  USART_MBUS + USART_CR3, (1 << 12), // OVRDIS
  USART_MBUS + USART_CR1, (USART_CR1_UE | USART_CR1_TE | USART_CR1_RE),
  USART_MBUS + USART_CR2, 0,
  USART_MBUS + USART_ICR, 0xffffffff,

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
  GPIO_BITQUAD(2, 1) | GPIO_BITQUAD(3, 1) |
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

  // Set PB6 and PB7 into PULLUP
  GPIO_PUPDR(PB),
#ifdef USART1_PB6_PB7
  GPIO_BITPAIR(6, 1) | GPIO_BITPAIR(7, 1) |
#endif
#ifdef USART3_PB8_PB9
  GPIO_BITPAIR(8, 1) | GPIO_BITPAIR(9, 1) |
#endif
  0,

  GPIO_AFRH(PB),
#ifdef USART3_PB8_PB9
  GPIO_BITQUAD(0, 4) | GPIO_BITQUAD(1, 4) |
#endif
  0,

  GPIO_MODER(PB),
#ifdef USART1_PB6_PB7
  GPIO_BITPAIR(6, 2) | GPIO_BITPAIR(7, 2) |
#endif
#ifdef USART3_PB8_PB9
  GPIO_BITPAIR(8, 2) | GPIO_BITPAIR(9, 2) |
#endif
  GPIO_MODER_PB,

  // -----------------------------------------------------
  // GPIO PORT C & D
  // -----------------------------------------------------

#ifdef GPIO_MODER_PC
  GPIO_MODER(PC),  GPIO_MODER_PC,
#endif
#ifdef GPIO_MODER_PD
  GPIO_MODER(PD),  GPIO_MODER_PD,
#endif
};


__attribute__((section("bldata")))
static const char appname[] = APPNAME;

//======================================================================
// CRC-32
//======================================================================
#define CRC_BASE 0x40023000

#define CRC_DR   0x00
#define CRC_IDR  0x04
#define CRC_CR   0x08
#define CRC_INIT 0x10
#define CRC_POL  0x14


static uint32_t __attribute__((section("bltext"),noinline))
bl_crc32(const void *data, size_t len)
{
  size_t i = 0;
  const uint8_t *d8 = data;

  reg_wr(CRC_BASE + CRC_INIT, 0xffffffff);
  reg_wr(CRC_BASE + CRC_CR, 0xa1);

  for(i = 0; i + 4 <= len; i += 4)
    reg_wr(CRC_BASE + CRC_DR, __builtin_bswap32(*(uint32_t *)&d8[i]));

  for(; i + 2 <= len; i += 2)
    reg_wr16(CRC_BASE + CRC_DR, __builtin_bswap16(*(uint16_t *)&d8[i]));

  for(; i < len; i++)
    reg_wr8(CRC_BASE + CRC_DR, d8[i]);

  return reg_rd(CRC_BASE + CRC_DR);
}


static void __attribute__((section("bltext"),noinline))
bl_crc32_block(const void *data)
{
  const uint32_t *u32 = data;
  for(int i = 0; i < BLOCKSIZE / 4; i++)
    reg_wr(CRC_BASE + CRC_DR, __builtin_bswap32(u32[i]));
}


//======================================================================
// CRC-4
//======================================================================
__attribute__((section("bldata")))
static const uint8_t crc4_tab[] = {
	0x0, 0x7, 0xe, 0x9, 0xb, 0xc, 0x5, 0x2,
	0x1, 0x6, 0xf, 0x8, 0xa, 0xd, 0x4, 0x3,
};



__attribute__((section("bltext"),noinline))
static uint8_t
crc4(uint8_t c, uint8_t *data, size_t nibbles)
{
  for(size_t i = 0; i < nibbles; i++) {
    c = crc4_tab[c ^ ((data[i >> 1] >> (i & 1 ? 4 : 0)) & 0xf)];
  }
  return c;
}


//======================================================================
// MBUS
//======================================================================

#define SP_FF   0x1
#define SP_LF   0x2
#define SP_ESEQ 0x4
#define SP_SEQ  0x8
#define SP_CTS  0x10
#define SP_MORE 0x20

__attribute__((section("bltext"),noinline))
static int
mbus_recv_packet(ota_workmem_t *o)
{
  reg_wr(USART_MBUS + USART_BRR, 139); // 115200
  const int h0 = mbus_recv_byte(RECV_TIMEOUT_SLOW);
  if(h0 < 0)
    return 2;
  const int h1 = mbus_recv_byte(RECV_TIMEOUT_FAST);
  if(h1 < 0)
    return 3;

  uint8_t hdr[2] = {h0,h1};
  if(crc4(0, hdr, 4))
    return 4;

  o->rx_buf[0] = h0 & 0x3f;
  const int payload_len = (h0 >> 6) | ((h1 & 0xf) << 2);
  reg_wr(USART_MBUS + USART_BRR, 16); // 1 MBAUD
  for(int i = 0; i < payload_len; i++) {
    const int p = mbus_recv_byte(RECV_TIMEOUT_FAST);
    if(p < 0)
      return 5;
    o->rx_buf[i + 1] = p;
  }
  o->rx_len = payload_len + 1 - 4;
  if(bl_crc32(o->rx_buf, payload_len + 1)) {
    return 6;
  }
  return 0;
}


__attribute__((section("bltext"),noinline))
static void
mbus_tx_packet(ota_workmem_t *o)
{
  uint32_t crc = bl_crc32(o->tx_buf, o->tx_len);
  uint32_t len = o->tx_len;
  o->tx_buf[len + 0] = crc;
  o->tx_buf[len + 1] = crc >> 8;
  o->tx_buf[len + 2] = crc >> 16;
  o->tx_buf[len + 3] = crc >> 24;
  len += 4;

  // Wait for BUSY to clear
  while((reg_rd(USART_MBUS + USART_SR) & (1 << 16))) {}

  reg_wr(GPIO_BSRR(TXE_PORT), (1 << TXE_BIT)); // Enable TXE

  reg_wr(USART_MBUS + USART_BRR, 139); // 115200

  int payload_len = len - 1;
  uint8_t hdr[2] = {o->tx_buf[0] | (payload_len << 6), payload_len >> 2};
  uint8_t c = crc4(0, hdr, 3);
  hdr[1] |= c << 4;

  mbus_send_byte(hdr[0]);
  mbus_send_byte(hdr[1]);

  // Delay ~100µs
  for(int i = 0; i < 250; i++) {
    asm volatile("nop");
  }

  reg_wr(USART_MBUS + USART_BRR, 16); // 1 MBAUD
  for(int i = 0; i < payload_len; i++) {
    mbus_send_byte(o->tx_buf[i + 1]);
  }
  reg_wr(GPIO_BSRR(TXE_PORT), (1 << (16 + TXE_BIT))); // Disable TXE
}


//======================================================================
// Flash
//======================================================================

#define FLASH_BASE 0x40022000

#define FLASH_KEYR    (FLASH_BASE + 0x08)
#define FLASH_SR      (FLASH_BASE + 0x10)
#define FLASH_CR      (FLASH_BASE + 0x14)

static void __attribute__((section("bltext"),noinline))
flash_unlock(void)
{
  reg_wr(FLASH_KEYR, 0x45670123);
  reg_wr(FLASH_KEYR, 0xCDEF89AB);
}

static void __attribute__((section("bltext"),noinline))
flash_lock(void)
{
  reg_wr(FLASH_CR, 0x80000000);
}

static void __attribute__((section("bltext"),noinline))
flash_erase_sector(int sector)
{

  reg_wr(FLASH_CR, 0x2 | (sector << 3));
  reg_set_bit(FLASH_CR, 16);
  while(reg_rd(FLASH_SR) & (1 << 16)) {}
}

static void __attribute__((section("bltext"),noinline))
flash_write_block(uint32_t block, const uint8_t *s)
{
  volatile uint32_t *dst = (void *)0x8000000 + block * BLOCKSIZE;
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
// OTA packet handler
//======================================================================

static int __attribute__((section("bltext"),noinline))
handle_pkt(ota_workmem_t *o)
{
  if(o->rx_len < 4) {
    return 0;
  }
  if(o->rx_buf[0] != o->local_addr) {
    return 0; // Not for us
  }

  uint16_t flow = o->rx_buf[2] | ((o->rx_buf[1] & 0x60) << 3);
  uint8_t remote_addr = o->rx_buf[1] & 0x1f;

  if(o->block == -2) {

    if(o->rx_len != 7)
      return 0; // Not expected length
    if(!(o->rx_buf[1] & 0x80))
      return 0; // Not INIT packet
    if(o->rx_buf[3] != 1 ||
       o->rx_buf[4] != 'o' || o->rx_buf[5] != 't' || o->rx_buf[6] != 'a')
      return 0; // Not SEQPKT for OTA

    o->flow = flow;
    o->remote_addr = remote_addr;

    o->tx_buf[0] = o->remote_addr;
    o->tx_buf[1] = ((o->flow >> 3) & 0x60) | o->local_addr;
    o->tx_buf[2] = o->flow;
    o->tx_buf[3] = SP_CTS | SP_FF | SP_LF;
    o->tx_buf[4] = 0;   // Flags
    o->tx_buf[5] = 'r'; // RAW mode
    o->tx_buf[6] = BLOCKSIZE;
    o->tx_buf[7] = 2;   // Non-writable space (2k)

    for(size_t i = 0; i < 20; i++) {
      o->tx_buf[8 + i] = 0;
    }

    for(size_t i = 0; i < sizeof(appname); i++) {
      o->tx_buf[8 + 20 + i] = appname[i];
    }

    o->tx_len = 8 + 20 + sizeof(appname);
    o->block = -1;
    mbus_tx_packet(o);
    return 0;
  }

  if(o->flow != flow || o->remote_addr != remote_addr) {
    console_send('F');
    return 0;
  }

  reg_wr(IWDG_KR, 0xAAAA);

  if(o->tx_len > 4) {

    if(!(o->tx_buf[3] & SP_SEQ) == !(o->rx_buf[3] & SP_ESEQ)) {
      // Retransmit last packet
      mbus_tx_packet(o);
      return 0;
    }
    o->tx_len = 4;
    o->tx_buf[3] &= ~(SP_FF | SP_LF);
  }

  if(!(o->rx_buf[3] & SP_SEQ) != !(o->tx_buf[3] & SP_ESEQ)) {
    // Did not receive expected frame, ask for retransmit
    mbus_tx_packet(o);
    return 0;
  }

  o->tx_buf[3] ^= SP_ESEQ;

  int ret = 0;

  if(o->block == -1) {
    if(o->rx_len != 12) {
      o->block = -2;
      return 0;
    }
    uint32_t *hdr = (uint32_t *)&o->rx_buf[4];
    o->num_blocks = hdr[0];
    o->image_crc = hdr[1];

    o->block++;

  } else if(o->block < o->num_blocks && o->rx_len == 4 + BLOCKSIZE) {

    flash_unlock();

    if((o->block & 0x3f) == 0) {
      int sector = o->block / BLOCKS_PER_SECTOR;
      flash_erase_sector(sector + 1);
    }

    if(o->block == 0) {
      // Block 0 is written last to signal that we've written everything
      for(int i = 0; i < BLOCKSIZE; i++)
        o->block0[i] = o->rx_buf[4 + i];
    } else {
      flash_write_block(o->block + BLOCKS_PER_SECTOR, o->rx_buf + 4);
    }

    flash_lock();

    o->block++;
    if(o->block == o->num_blocks) {
      // Done

      // Compute CRC over entire image
      reg_wr(CRC_BASE + CRC_INIT, 0xffffffff);
      reg_wr(CRC_BASE + CRC_CR, 0xa1);
      bl_crc32_block(o->block0);
      for(int i = 1; i < o->num_blocks; i++) {
        bl_crc32_block((void *)0x08000800 + i * BLOCKSIZE);
      }

      ret = 1;

      if(reg_rd(CRC_BASE + CRC_DR) == o->image_crc) {
        // Image checksum ok
        // Write first block
        flash_unlock();
        flash_write_block(BLOCKS_PER_SECTOR, o->block0);
        flash_lock();
        o->tx_buf[4] = 0; // Status code
      } else {
        o->tx_buf[4] = 18; // CHECKSUM_ERROR
      }

      // And send reply
      o->tx_len = 5;
      o->tx_buf[3] |= SP_FF | SP_LF | SP_SEQ;
    }
  }
  mbus_tx_packet(o);
  return ret;
}



__attribute__((section("bldata")))
static const char welcomestr[] = "\nMBUS Bootloader ready\n";

__attribute__((section("bldata")))
static const char hfstr[] = "HARD FAULT\n";



static void  __attribute__((section("bltext"),noinline))
reboot(void)
{
  static volatile uint32_t *const AIRCR  = (volatile uint32_t *)0xe000ed0c;
  *AIRCR = 0x05fa0004;
  while(1) {}
}


static void __attribute__((section("bltext"),noinline))
mbus_ota(ota_workmem_t *o)
{
  for(size_t i = 0; i < sizeof(reginit) / sizeof(reginit[0]); i += 2) {
    reg_wr(reginit[i + 0], reginit[i + 1]);
  }
  console_send_string(welcomestr);
  while(1) {
    int r = mbus_recv_packet(o);
    if(!r) {

      if(handle_pkt(o)) {
        break;
      }
    } else if(r != 2){
      console_send('E');
      console_send_nibble(r);
    }
  }
  reboot();
}


extern uint32_t vectors[];

void __attribute__((section("bltext"),noinline,noreturn)) bl_start(void)
{
  if(vectors[1] == 0xffffffff) {

    reg_wr(IWDG_KR, 0x5555);
    reg_wr(IWDG_RLR, 128 * 5);  // 5 seconds timeout
    reg_wr(IWDG_PR, 6);  // Prescaler (/256) for 32768 -> 128
    reg_wr(IWDG_KR, 0xAAAA);
    reg_wr(IWDG_KR, 0xCCCC);

    ota_workmem_t *o = (ota_workmem_t *)0x20000200;
    o->block = -2;
    o->local_addr = bl_mbus_local_addr();

    mbus_ota(o);
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
  console_send_string(hfstr);
  while(1) {}
}


void __attribute__((section("bltext"),noinline))
bl_ota_start(uint32_t flow_header, uint32_t num_blocks, uint32_t image_crc)
{
  ota_workmem_t *o = (ota_workmem_t *)0x20000200;

  o->tx_buf[0] = flow_header;
  o->tx_buf[1] = flow_header >> 8;
  o->tx_buf[2] = flow_header >> 16;
  o->tx_buf[3] = flow_header >> 24;
  o->tx_len = 4;

  o->remote_addr = o->tx_buf[0];
  o->flow = o->tx_buf[2] | ((o->tx_buf[1] & 0x60) << 3);
  o->local_addr = o->tx_buf[1] & 0x1f;

  o->block = 0;
  o->num_blocks = num_blocks;
  o->image_crc = image_crc;
  mbus_ota(o);
}

void __attribute__((section("bltext"), naked))
bl_ota_start0(uint32_t flow_header, uint32_t num_blocks, uint32_t image_crc)
{
  asm volatile("cpsid i");
  asm volatile("msr control, %0" :: "r"(0));
  asm volatile("msr msp, %0" :: "r"(0x20000200));
  bl_ota_start(flow_header, num_blocks, image_crc);
}
