#include <stdlib.h>
#include <unistd.h>
#include <mios/mios.h>
#include <mios/flash.h>

#include "stm32f4_reg.h"

#include "systick.h"
#include "irq.h"

#include <stdio.h>


static int
get_sector_size(unsigned int sector)
{
  static volatile uint16_t *const FLASH_SIZE = (volatile uint16_t *)0x1fff7a22;

  if(sector < 4)
    return 16 * 1024;
  if(sector == 4)
    return 64 * 1024;

  sector -= 4;
  if(sector * 128 >= *FLASH_SIZE)
    return 0;
  return 128 * 1024;
}

static size_t
get_sector_offset(unsigned int sector)
{
  size_t off = 0;
  for(unsigned int i = 0; i < sector; i++) {
    int s = get_sector_size(i);
    if(s == 0)
      panic("Invalid flash sector %d", sector);
    off += s;
  }
  return off;
}


#define FLASH_BASE 0x40023c00

#define FLASH_KEYR    (FLASH_BASE + 0x04)
#define FLASH_IOTKEYR (FLASH_BASE + 0x08)
#define FLASH_SR      (FLASH_BASE + 0x0c)
#define FLASH_CR      (FLASH_BASE + 0x10)

#define IWDG_KR  0x40003000

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




static size_t
flash_get_sector_size(const struct flash_iface *fif, int sector)
{
  return get_sector_size(sector);
}


static void __attribute__((section("ramcode"),noinline))
flash_erase_sector0(int sector)
{
  reg_wr(FLASH_CR, 0x2 | (sector << 3));
  reg_set_bit(FLASH_CR, 16);
  flash_wait_ready();
}


static error_t
flash_erase_sector(const struct flash_iface *fif, int sector)
{
  if(flash_get_sector_size(fif, sector) == 0)
    return ERR_INVALID_ID;

  int q = irq_forbid(IRQ_LEVEL_ALL);
  flash_unlock();
  flash_erase_sector0(sector);
  flash_lock();
  irq_permit(q);
  return 0;
}


static error_t
flash_write(const struct flash_iface *fif, int sector,
            size_t offset, const void *data, size_t len)
{
  const size_t size = flash_get_sector_size(fif, sector);
  if(size == 0)
    return ERR_INVALID_ID;

  if(offset + len >= size * 1024)
    return ERR_INVALID_ADDRESS;

  const size_t soffset = get_sector_offset(sector);

  const uint8_t *d8 = data;
  volatile uint8_t *addr = (uint8_t *)(0x8000000 + offset + soffset);

  flash_unlock();

  while(reg_rd(FLASH_SR) & (1 << 16)) {
  }

  reg_wr(FLASH_CR, 0x1 | (0 << 8));

  for(size_t i = 0; i < len; i++)
    addr[i] = d8[i];

  flash_lock();
  return 0;
}


static const void *
flash_get_addr(const struct flash_iface *fif, int sector)
{
  const size_t size = flash_get_sector_size(fif, sector);
  if(size == 0)
    return NULL;

  const size_t soffset = get_sector_offset(sector);
  return (const void *)(0x8000000 + soffset);
}



static error_t
flash_read(const struct flash_iface *fif, int sector,
           size_t offset, void *data, size_t len)
{
  const size_t size = flash_get_sector_size(fif, sector);
  if(size == 0)
    return ERR_INVALID_ID;

  if(offset + len >= size * 1024)
    return ERR_INVALID_ADDRESS;

  const size_t soffset = get_sector_offset(sector);
  uint8_t *d8 = data;
  volatile uint8_t *addr = (uint8_t *)(0x8000000 + offset + soffset);

  for(size_t i = 0; i < len; i++)
    d8[i] = addr[i];

  return 0;
}


static error_t
flash_compare(const struct flash_iface *fif, int sector,
              size_t offset, const void *data, size_t len)
{
  const size_t size = flash_get_sector_size(fif, sector);
  if(size == 0)
    return ERR_INVALID_ID;

  if(offset + len >= size * 1024)
    return ERR_INVALID_ADDRESS;

  const size_t soffset = get_sector_offset(sector);
  const uint8_t *d8 = data;
  volatile uint8_t *addr = (uint8_t *)(0x8000000 + offset + soffset);

  for(size_t i = 0; i < len; i++) {
    if(d8[i] != addr[i])
      return ERR_MISMATCH;
  }
  return 0;
}



static void  __attribute__((section("ramcode"),noinline))
flash_multi_write0(uint32_t erase_mask,
                   const flash_multi_write_chunks_t *chunks,
                   const void *src_base,
                   int flags)
{
  for(int i = 0 ; i < 32; i++) {
    if((1 << i) & erase_mask)
      flash_erase_sector0(i);
  }

  while(reg_rd(FLASH_SR) & (1 << 16)) {
  }

  reg_wr(FLASH_CR, 0x1 | (0 << 8));

  for(int i = 0; i < chunks->num_chunks; i++) {
    const flash_multi_write_chunk_t *c = &chunks->chunks[i];

    reg_wr(IWDG_KR, 0xAAAA);

    const uint8_t *src = c->src_offset + src_base;
    const size_t len = c->length;
    volatile uint8_t *dst = (uint8_t *)c->dst_offset;

    for(size_t i = 0; i < len; i++)
      dst[i] = src[i];
  }

  while(reg_rd(FLASH_SR) & (1 << 16)) {
  }

  if(flags & FLASH_MULTI_WRITE_CPU_REBOOT) {
    static volatile uint32_t *const AIRCR = (volatile uint32_t *)0xe000ed0c;
    *AIRCR = 0x05fa0004;
    while(1) {}
  }

}



static void
flash_multi_write(const struct flash_iface *fif,
                  const flash_multi_write_chunks_t *chunks,
                  const void *src_base,
                  int flags)
{

  uint32_t erase_mask = 0;

  for(int i = 0; ; i++) {
    const size_t sec_size = get_sector_size(i);
    if(sec_size == 0)
      break;

    const size_t sec_off = get_sector_offset(i) + 0x08000000;

    for(int j = 0; j < chunks->num_chunks; j++) {
      const flash_multi_write_chunk_t *c = &chunks->chunks[j];
      const size_t start = c->dst_offset;
      const size_t end = start + (c->length - 1);
      if((start >= sec_off && start < sec_off + sec_size) ||
         (end >= sec_off && end < sec_off + sec_size)) {
        erase_mask |= (1 << i);
      }
    }
  }

  int q = irq_forbid(IRQ_LEVEL_ALL);
  flash_unlock();
  flash_multi_write0(erase_mask, chunks, src_base, flags);
  flash_lock();
  irq_permit(q);
}


static flash_sector_type_t
flash_get_sector_type(const struct flash_iface *fif, int sector)
{
  switch(sector) {
  case 0:
    return FLASH_SECTOR_TYPE_PROG;
  case 1:
  case 2:
    return FLASH_SECTOR_TYPE_PKV;
  case 3:
    return FLASH_SECTOR_TYPE_AVAIL;
  default:
    break;
  }

  const size_t size = get_sector_size(sector);
  if(size == 0)
    return 0;

  const size_t offset = get_sector_offset(sector);

  extern unsigned long _sdata;
  extern unsigned long _etext;
  extern unsigned long _edata;
  const size_t data_size = (void *)&_edata - (void *)&_sdata;
  void *end_of_prog = (void *)&_etext + data_size;

  void *begin_of_sector = (void *)0x8000000 + offset;

  if(end_of_prog > begin_of_sector)
    return FLASH_SECTOR_TYPE_PROG;

  return FLASH_SECTOR_TYPE_AVAIL;
}

static const flash_iface_t stm32f4_flash = {
  .get_sector_size = flash_get_sector_size,
  .get_sector_type = flash_get_sector_type,
  .erase_sector = flash_erase_sector,
  .write = flash_write,
  .read = flash_read,
  .compare = flash_compare,
  .get_addr = flash_get_addr,
  .multi_write = flash_multi_write,
};


const flash_iface_t *
flash_get_primary(void)
{
  return &stm32f4_flash;
}
