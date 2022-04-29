#include <mios/ota.h>
#include <mios/timer.h>
#include <mios/io.h>
#include <mios/suspend.h>

#include <malloc.h>
#include <string.h>
#include <unistd.h>
#include <assert.h>

#include "systick.h"
#include "irq.h"

#include "net/mbus/mbus.h"

#include "stm32g0_reg.h"
#include "stm32g0_clk.h"
#include "stm32g0_ota.h"
#include "stm32g0_flash.h"


static uint32_t ota_uart_addr;
static uint8_t ota_local_addr = 0xff;
static gpio_t ota_txe = GPIO_UNUSED;

void
stm32g0_ota_configure(uint32_t baseaddr, uint8_t local_addr, gpio_t txe)
{
  ota_uart_addr = baseaddr;
  ota_local_addr = local_addr;
  ota_txe = txe;
}

typedef struct ota_state {
  uint8_t os_rxbuf[32];
  uint8_t os_txpkt[10];
  uint32_t os_mbus_uart;
  ota_req_t os_req;
  uint8_t os_rxptr;
  uint8_t os_rxaddr;
  uint8_t os_ignore_err;
  gpio_t os_txe;
  timer_t os_launcher;
  void (*os_updater)(struct ota_state *os);
} ota_state_t;


#define FLASH_BASE 0x40022000

#define FLASH_KEYR    (FLASH_BASE + 0x08)
#define FLASH_SR      (FLASH_BASE + 0x10)
#define FLASH_CR      (FLASH_BASE + 0x14)

#define IWDG_BASE 0x40003000
#define IWDG_KR  (IWDG_BASE + 0x00)

#define USART_CR1  0x00
#define USART_CR2  0x04
#define USART_CR3  0x08
#define USART_BBR  0x0c
#define USART_SR   0x1c
#define USART_ICR  0x20
#define USART_RDR  0x24
#define USART_TDR  0x28

#define CRC_BASE 0x40023000

#define CRC_DR   0x00
#define CRC_IDR  0x04
#define CRC_CR   0x08
#define CRC_INIT 0x10
#define CRC_POL  0x14

#define GPIO_PORT_ADDR(x) (0x50000000 + ((x) * 0x400))
#define GPIO_BSRR(x)    (GPIO_PORT_ADDR(x) + 0x18)

static volatile unsigned int * const SYST_CSR = (unsigned int *)0xe000e010;

#define OTA_HELPER inline __attribute__((always_inline))



static void  OTA_HELPER __attribute__((unused))
ota_gpio_set_output(gpio_t gpio, int on)
{
  const int port = gpio >> 4;
  const int bit = gpio & 0xf;

  reg_set(GPIO_BSRR(port), 1 << (bit + !on * 16));
}


static int OTA_HELPER __attribute__((unused))
ota_getc(const ota_state_t *os)
{
  int c = 0;

  while(1) {

    if(*SYST_CSR & 0x10000) {
      c++;
      reg_wr(IWDG_KR, 0xAAAA);
      if(c == HZ) {
        return -1;
      }
    }
    uint32_t sr = reg_rd(os->os_mbus_uart + USART_SR);
    if(sr & (1 << 5)) {
      return reg_rd(os->os_mbus_uart + USART_RDR);
    }
  }
}

static uint32_t OTA_HELPER
ota_crc(const void *data, size_t len)
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


static void OTA_HELPER
ota_putc(const ota_state_t *os, uint8_t c)
{
  while(1) {
    const uint32_t sr = reg_rd(os->os_mbus_uart + USART_SR);
    if(sr & (1 << 7)) {
      reg_wr(os->os_mbus_uart + USART_TDR, c);
      return;
    }
  }
}


static void OTA_HELPER
ota_xmit(const ota_state_t *os, const uint8_t *pkt, size_t len)
{
  while(reg_rd(os->os_mbus_uart + USART_SR) & (1 << 16)) {
    reg_wr(IWDG_KR, 0xAAAA);
  }
  ota_gpio_set_output(os->os_txe, 1);
  ota_putc(os, 0x7e);
  for(size_t i = 0; i < len; i++) {
    uint8_t c = pkt[i];
    if(c == 0x7d || c == 0x7e) {
      ota_putc(os, 0x7d);
      c ^= 0x20;
    }
    ota_putc(os, c);
  }
  ota_putc(os, 0x7e);
  while(!(reg_rd(os->os_mbus_uart + USART_SR) & (1 << 6))) {}
  ota_gpio_set_output(os->os_txe, 0);
}


static void OTA_HELPER
ota_req(ota_state_t *os, uint32_t block, uint8_t last_err)
{
  if(last_err == 2) {
    os->os_ignore_err++;
    if(os->os_ignore_err < 3)
      return;
  }
  os->os_ignore_err = 0;
  os->os_txpkt[2] = block;
  os->os_txpkt[3] = block >> 8;
  os->os_txpkt[4] = block >> 16;
  os->os_txpkt[5] = last_err;
  const uint32_t crc = ota_crc(os->os_txpkt, 6);
  os->os_txpkt[6] = crc;
  os->os_txpkt[7] = crc >> 8;
  os->os_txpkt[8] = crc >> 16;
  os->os_txpkt[9] = crc >> 24;
  ota_xmit(os, os->os_txpkt, 10);
}

static int OTA_HELPER
ota_recv_pkt(ota_state_t *os, uint32_t expected_block)
{
  const uint8_t *rx = os->os_rxbuf;

  if(rx[0] != os->os_rxaddr)
    return 2;

  if(ota_crc(os->os_rxbuf, os->os_rxptr))
    return 3;

  const int len = os->os_rxptr - 4;
  if(len != 2 + 3 + 16)
    return 1;

  if(rx[1] != 0xd)
    return 4;

  uint32_t block = rx[2] | (rx[3] << 8) | (rx[4] << 16);

  if(block < expected_block) {
    // Old block, delay a bit
    while(!(*SYST_CSR & 0x10000)) {
    }
    reg_wr(IWDG_KR, 0xAAAA);
  }

  if(block != expected_block)
    return 5;

  rx += 5;

  reg_wr(FLASH_KEYR, 0x45670123);
  reg_wr(FLASH_KEYR, 0xCDEF89AB);

  if((block & 0x7f) == 0) {
    if(stm32g0_flash_erase_sector_ramcode(block >> 7)) {
      reg_wr(FLASH_CR, 0x80000000);
      return 8;
    }
  }

  volatile uint32_t *dst = (void *)0x8000000 + block * 16;

  reg_wr(FLASH_CR, 0x1);
  const uint32_t w0 = (rx[0x0] << 0) | (rx[0x1] << 8) | (rx[0x2] << 16) | (rx[0x3] << 24);
  const uint32_t w1 = (rx[0x4] << 0) | (rx[0x5] << 8) | (rx[0x6] << 16) | (rx[0x7] << 24);
  dst[0] = w0;
  dst[1] = w1;
  while(reg_rd(FLASH_SR) & (1 << 16)) {
  }

  reg_wr(FLASH_CR, 0x1);
  const uint32_t w2 = (rx[0x8] << 0) | (rx[0x9] << 8) | (rx[0xa] << 16) | (rx[0xb] << 24);
  const uint32_t w3 = (rx[0xc] << 0) | (rx[0xd] << 8) | (rx[0xe] << 16) | (rx[0xf] << 24);
  dst[2] = w2;
  dst[3] = w3;
  while(reg_rd(FLASH_SR) & (1 << 16)) {
  }

  reg_wr(FLASH_CR, 0x80000000);

  return 0;
}

static void OTA_HELPER
ota_send_done(ota_state_t *os, uint8_t err)
{
  os->os_txpkt[2] = 0xff;
  os->os_txpkt[3] = 0xff;
  os->os_txpkt[4] = 0xff;
  os->os_txpkt[5] = err;
  const uint32_t crc = ota_crc(os->os_txpkt, 6);
  os->os_txpkt[6] = crc;
  os->os_txpkt[7] = crc >> 8;
  os->os_txpkt[8] = crc >> 16;
  os->os_txpkt[9] = crc >> 24;
  ota_xmit(os, os->os_txpkt, 10);
  ota_xmit(os, os->os_txpkt, 10);
  ota_xmit(os, os->os_txpkt, 10);
}


static int OTA_HELPER
ota_maybe_completed(ota_state_t *os)
{
  uint32_t our_crc = ota_crc((void *)0x8000000, os->os_req.blocks * 16);
  if(our_crc != os->os_req.crc) {
    return 6;
  }
  ota_send_done(os, 0);
  for(int i = 0; i < 100000; i++) {
    asm volatile("nop");
  }
  // REBOOT
  static volatile uint32_t *const AIRCR  = (volatile uint32_t *)0xe000ed0c;
  *AIRCR = 0x05fa0004;
  while(1) {}
}



void  __attribute__((section("ota_flasher"),noinline))
ota_loop(ota_state_t *os)
{
  uint32_t block = 0;
  uint8_t last_err = 0;

  while(1) {
    if(block == os->os_req.blocks) {

      last_err = ota_maybe_completed(os);
      // If this returns, CRC failed, start over
      block = 0;
    }
    ota_req(os, block, last_err);
    os->os_rxptr = 0;
    reg_wr(os->os_mbus_uart + USART_CR1, (1 << 0) | (1 << 3) | (1 << 2));
    while(1) {
      int c = ota_getc(os);
      if(c == -1) {
        break;
      }

      if(c == 0x7e) {
        if(os->os_rxptr) {

          last_err = ota_recv_pkt(os, block);
          if(!last_err)
            block++;
          os->os_rxptr = 0;
          break;
        }
        continue;
      }
      if(c == 0x7d) {
        c = ota_getc(os);
        if(c == -1)
          break;
        c ^= 0x20;
      }
      if(os->os_rxptr == sizeof(os->os_rxbuf))
        break;
      os->os_rxbuf[os->os_rxptr] = c;
      os->os_rxptr++;
    }
    reg_wr(os->os_mbus_uart + USART_CR1, (1 << 0) | (1 << 3));
  }
}


static void
updater_prepare(void *opaque, uint64_t expire)
{
  ota_state_t *os = opaque;

  irq_forbid(IRQ_LEVEL_ALL);
  fini();
  clk_enable(CLK_CRC);

  reg_wr(os->os_mbus_uart + USART_CR1, 0);
  reg_wr(os->os_mbus_uart + USART_CR3, (1 << 12)); // OVRDIS);
  reg_wr(os->os_mbus_uart + USART_CR1, (1 << 0) | (1 << 3));
  reg_wr(os->os_mbus_uart + USART_CR2, 0);
  reg_wr(os->os_mbus_uart + USART_ICR, 0xffffffff);

  ota_gpio_set_output(os->os_txe, 0);
  os->os_updater(os);
}



#include <stdio.h>

static volatile uint16_t *const FLASH_SIZE = (volatile uint16_t *)0x1fff75e0;

static int
get_ota_trampoline_sector(void)
{
  return (*FLASH_SIZE / 2) - 2;
}

static void *
get_ota_trampoline_addr(void)
{
  return (void *)0x08000000 + get_ota_trampoline_sector() * 2048;
}



static void OTA_HELPER
flash_write_quad(volatile uint32_t *dst, const void *src)
{
  const uint32_t *s32 = src;
  reg_wr(FLASH_CR, 0x1);
  dst[0] = s32[0];
  dst[1] = s32[1];

  while(reg_rd(FLASH_SR) & (1 << 16)) {
  }
}

error_t
rpc_ota(const ota_req_t *in, void *out, size_t in_size)
{
  if(in->type != OTA_TYPE_RAW)
    return ERR_MISMATCH;

  if(!ota_uart_addr || ota_txe == GPIO_UNUSED || ota_local_addr == 0xff)
    return ERR_NOT_READY;

  extern unsigned long _ota_flasher_begin;
  extern unsigned long _ota_flasher_end;
  void *src = &_ota_flasher_begin;
  void *stop = &_ota_flasher_end;

  if(stop - src > 2048)
    return ERR_NO_FLASH_SPACE;

  ota_state_t *os = xalloc(1, sizeof(ota_state_t), MEM_MAY_FAIL);
  if(os == NULL)
    return ERR_NO_BUFFER;
  memset(os, 0, sizeof(ota_state_t));
  memcpy(&os->os_req, in, sizeof(ota_req_t));
  os->os_updater = (void *)((intptr_t)get_ota_trampoline_addr() | (intptr_t)1);

  os->os_launcher.t_cb = updater_prepare;
  os->os_launcher.t_opaque = os;

  os->os_mbus_uart = ota_uart_addr;
  os->os_txe = ota_txe;
  uint8_t our_addr = ota_local_addr;

  os->os_txpkt[0] = (our_addr << 4) | in->hostaddr;
  os->os_rxaddr   = (in->hostaddr << 4) | our_addr;
  os->os_txpkt[1] = MBUS_OP_OTA_XFER;


  const int sector = get_ota_trampoline_sector();

  wakelock_acquire();

  int q = irq_forbid(IRQ_LEVEL_ALL);
  reg_wr(FLASH_KEYR, 0x45670123);
  reg_wr(FLASH_KEYR, 0xCDEF89AB);

  error_t err = stm32g0_flash_erase_sector_ramcode(sector);
  if(!err) {
    void *dst = get_ota_trampoline_addr();
    while(src < stop) {
      flash_write_quad(dst, src);
      src += 8;
      dst += 8;
    }
    reg_wr(FLASH_CR, 0x80000000);
    timer_arm_abs(&os->os_launcher, clock_get() + 100000);
  } else {
    wakelock_release();
  }
  irq_permit(q);
  return err;
}

error_t
rpc_otamode(const void *in, uint8_t *out, size_t in_size)
{
  out[0] = OTA_TYPE_RAW;
  return 0;
}
