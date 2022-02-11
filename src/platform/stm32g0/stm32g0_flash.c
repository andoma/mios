#include "stm32g0_flash.h"

#include <stdlib.h>
#include <unistd.h>
#include <mios/mios.h>
#include <sys/param.h>

#include "stm32g0_reg.h"
#include "systick.h"
#include "irq.h"
#include "util/crc32.h"

#include <string.h>
#include <stdio.h>


static volatile uint16_t *const FLASH_SIZE = (volatile uint16_t *)0x1fff75e0;

static int
get_p13n_sector(void)
{
  return (*FLASH_SIZE / 2) - 1;
}

static void *
get_p13n_addr(void)
{
  return (void *)0x08000000 + get_p13n_sector() * 2048;
}


#define FLASH_BASE 0x40022000

#define FLASH_KEYR    (FLASH_BASE + 0x08)
#define FLASH_SR      (FLASH_BASE + 0x10)
#define FLASH_CR      (FLASH_BASE + 0x14)

#define IWDG_BASE 0x40003000
#define IWDG_KR  (IWDG_BASE + 0x00)

static volatile unsigned int * const SYST_CSR = (unsigned int *)0xe000e010;

static inline void __attribute__((always_inline))
flash_wait_ready(void)
{
  // All interrupts are off, and we need to check if systick
  // wraps since a flash erase of a 128k sector takes as much
  // as 2 seconds

  extern uint64_t clock;
  while(reg_rd(FLASH_SR) & (1 << 16)) {
    if(*SYST_CSR & 0x10000) {
      clock += 1000000 / HZ;
    }
    reg_wr(IWDG_KR, 0xAAAA);
  }
}

static void
flash_unlock(void)
{
  reg_wr(FLASH_KEYR, 0x45670123);
  reg_wr(FLASH_KEYR, 0xCDEF89AB);
}

static void
flash_lock(void)
{
  reg_wr(FLASH_CR, 0x80000000);
}


static void __attribute__((section("ramcode"),noinline))
flash_erase_sector0(int sector)
{
  reg_wr(FLASH_CR, 0x2 | (sector << 3));
  reg_set_bit(FLASH_CR, 16);
  flash_wait_ready();
}


static error_t
flash_erase_sector(int sector)
{
  int q = irq_forbid(IRQ_LEVEL_ALL);
  flash_unlock();
  flash_erase_sector0(sector);
  flash_lock();
  irq_permit(q);
  return 0;
}


typedef struct {
  uint32_t size;
  uint32_t crc;
  uint8_t payload[0];
} p13n_hdr_t;

#define P13N_MAX_SIZE (2048 - sizeof(p13n_hdr_t))

const void *
stm32g0_p13n_get(size_t len)
{
  const p13n_hdr_t *p = get_p13n_addr();

  if(p->size > 2048 - 8)
    return NULL;

  if(p->size != len)
    return NULL;

  if(~crc32(0, p->payload, p->size) != p->crc)
    return NULL;

  return &p->payload[0];
}


static error_t
flash_write_quad(volatile uint32_t *dst, const void *src)
{
  const uint32_t *s32 = src;

  int q = irq_forbid(IRQ_LEVEL_ALL);
  reg_wr(FLASH_CR, 0x1);

  dst[0] = s32[0];
  dst[1] = s32[1];

  while(reg_rd(FLASH_SR) & (1 << 16)) {
  }
  irq_permit(q);
  return 0;
}


error_t
stm32g0_p13n_put(const void *data, size_t len)
{
  if(len > P13N_MAX_SIZE)
    return ERR_NO_FLASH_SPACE;
  
  error_t err = flash_erase_sector(get_p13n_sector());
  if(!err) {
    void *dst = get_p13n_addr();
    p13n_hdr_t p;
    p.size = len;
    p.crc = ~crc32(0, data, len);

    flash_unlock();

    err = flash_write_quad(dst, &p);
    dst += 8;
    if(!err) {
      uint8_t buf[8] = {};

      while(len > 0) {

        int csize = MIN(len, 8);
        memcpy(buf, data, csize);
        err = flash_write_quad(dst, buf);
        if(err)
          break;
        dst += 8;
        len -= csize;
      }
    }
    flash_lock();
  }
  return err;
}




