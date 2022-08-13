#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <mios/mios.h>
#include <mios/flash.h>

#include "stm32wb_reg.h"
#include "systick.h"
#include "irq.h"

#define FLASH_BASE 0x58004000

#define FLASH_KEYR    (FLASH_BASE + 0x08)
#define FLASH_SR      (FLASH_BASE + 0x10)
#define FLASH_CR      (FLASH_BASE + 0x14)


#define NUM_SECTORS 192


static inline error_t __attribute__((always_inline))
flash_wait_ready(void)
{
  int timeout = 0;
  while(reg_rd(FLASH_SR) & (1 << 16)) {
    if(clock_unwrap()) {
      timeout++;
      if(timeout == HZ)
        return ERR_FLASH_TIMEOUT;
    }
  }
  return 0;
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


error_t __attribute__((section("ramcode"),noinline))
flash_erase_sector0(int sector)
{
  reg_wr(FLASH_CR, 0x2 | (sector << 3));
  reg_set_bit(FLASH_CR, 16);
  return flash_wait_ready();
}



static error_t
flash_check_error(void)
{
  uint32_t sr = reg_rd(FLASH_SR);
  reg_wr(FLASH_SR, sr);
  if(!(sr & 0x43ff))
     return 0;
  if(sr & (1 << 14)) {
    return ERR_READ_PROTECTED;
  }
  if(sr & (1 << 6)) {
    return ERR_INVALID_LENGTH;
  }
  if(sr & (1 << 4)) {
    return ERR_WRITE_PROTECTED;
  }
  if(sr & (1 << 3)) {
    return ERR_NOT_READY;
  }
  return ERR_FLASH_HW_ERROR;
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

  error_t err = flash_check_error();
  irq_permit(q);
  return err;
}



static void  __attribute__((section("ramcode"),noinline))
flash_multi_write0(const uint32_t *erase_mask,
                   const flash_multi_write_chunks_t *chunks,
                   const void *src_base,
                   int flags)
{
  for(int i = 0 ; i < NUM_SECTORS; i++) {
    if((1 << (i & 31)) & erase_mask[i / 32])
      flash_erase_sector0(i);
  }

  while(reg_rd(FLASH_SR) & (1 << 16)) {
  }

  for(int i = 0; i < chunks->num_chunks; i++) {
    const flash_multi_write_chunk_t *c = &chunks->chunks[i];
    const uint32_t *src = c->src_offset + src_base;
    const size_t len = c->length / 8;
    volatile uint32_t *dst = (uint32_t *)c->dst_offset;

    for(size_t i = 0; i < len; i++) {
      reg_wr(FLASH_CR, 0x1);
      dst[i * 2 + 0] = src[i * 2 + 0];
      dst[i * 2 + 1] = src[i * 2 + 1];

      while(reg_rd(FLASH_SR) & (1 << 16)) {
      }
    }
  }

  if(flags & FLASH_MULTI_WRITE_CPU_REBOOT) {
    static volatile uint32_t *const AIRCR = (volatile uint32_t *)0xe000ed0c;
    *AIRCR = 0x05fa0004;
    while(1) {}
  }
}


//========================================================================

static size_t
flash_get_sector_size(const struct flash_iface *fif, int sector)
{
  if(sector >= NUM_SECTORS)
    return 0;

  return 4096;
}


static size_t
flash_get_sector_offset(const struct flash_iface *fif, unsigned int sector)
{
  return sector * 4096;
}


static flash_sector_type_t
flash_get_sector_type(const struct flash_iface *fif, int sector)
{
  if(sector >= NUM_SECTORS)
    return 0;

  size_t offset = flash_get_sector_offset(fif, sector);

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
  size_t sector_size = flash_get_sector_size(fif, sector);
  if(sector_size == 0)
    return ERR_INVALID_ID;

  if(offset + len > sector_size)
    return ERR_INVALID_LENGTH;

  if(len & 7)
    return ERR_INVALID_LENGTH;

  size_t quads = len / 8;

  flash_unlock();
  uint32_t da = 0x8000000 + sector * 4096 + offset;

  error_t err = 0;

  while(quads > 0 && !err) {
    err = flash_write_quad((uint32_t *)da, data);
    da += 8;
    data += 8;
    quads--;
  }

  flash_lock();
  return err;
}


static error_t
flash_read(const struct flash_iface *fif, int sector,
           size_t offset, void *data, size_t len)
{
  return ERR_NOT_IMPLEMENTED;
}


static error_t
flash_compare(const struct flash_iface *fif, int sector,
              size_t offset, const void *data, size_t len)
{
  return ERR_NOT_IMPLEMENTED;
}


static const void *
flash_get_addr(const struct flash_iface *fif, int sector)
{
  const size_t size = flash_get_sector_size(fif, sector);
  if(size == 0)
    return NULL;

  const size_t soffset = flash_get_sector_offset(fif, sector);
  return (const void *)(0x8000000 + soffset);
}




static void
flash_multi_write(const struct flash_iface *fif,
                  const flash_multi_write_chunks_t *chunks,
                  const void *src_base,
                  int flags)
{
  uint32_t erase_mask[NUM_SECTORS / 32] = {};

  for(int j = 0; j < chunks->num_chunks; j++) {
    const flash_multi_write_chunk_t *c = &chunks->chunks[j];
    const size_t start = c->dst_offset;

    if(start & 7) {
      panic("Unaligned flash write not supported");
    }
  }

  for(int i = 0; ; i++) {
    const size_t sec_size = flash_get_sector_size(fif, i);
    if(sec_size == 0)
      break;

    const size_t sec_off = flash_get_sector_offset(fif, i) + 0x08000000;

    for(int j = 0; j < chunks->num_chunks; j++) {
      const flash_multi_write_chunk_t *c = &chunks->chunks[j];
      const size_t start = c->dst_offset;
      const size_t end = start + (c->length - 1);
      if(start < sec_off + sec_size && end > sec_off) {
        erase_mask[i / 32] |= 1 << (i & 31);
      }
    }
  }

  int q = irq_forbid(IRQ_LEVEL_ALL);
  flash_unlock();
  flash_multi_write0(erase_mask, chunks, src_base, flags);
  flash_lock();
  irq_permit(q);
}

static const flash_iface_t stm32wb_flash = {
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
  return &stm32wb_flash;
}
