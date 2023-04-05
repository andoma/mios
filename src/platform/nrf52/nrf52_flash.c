#include <stdlib.h>
#include <unistd.h>
#include <mios/mios.h>
#include <mios/flash.h>

#include "nrf52_reg.h"
#include "nrf52_flash.h"

#include "irq.h"


static int
get_sector_size(unsigned int sector)
{
  const uint32_t flashsize = reg_rd(0x10000110); // in kB
  const uint32_t num_sectors = flashsize / 4;
  if(sector >= num_sectors)
    return 0;

  return 4096;
}

static size_t
get_sector_offset(unsigned int sector)
{
  return sector * 4096;
}


static inline void __attribute__((always_inline))
flash_wait_ready(void)
{
  while(!reg_rd(NVMC_READY)) {}
}

static size_t
flash_get_sector_size(const struct flash_iface *fif, int sector)
{
  return get_sector_size(sector);
}


static error_t
flash_erase_sector(const struct flash_iface *fif, int sector)
{
  reg_wr(NVMC_CONFIG, 2);
  reg_wr(NVMC_ERASEPAGE, sector * 4096);
  flash_wait_ready();
  reg_wr(NVMC_CONFIG, 0);
  return 0;
}


static error_t
flash_write(const struct flash_iface *fif, int sector,
            size_t offset, const void *data, size_t len)
{
  if((len & 3) || (offset & 3))
    return ERR_INVALID_ADDRESS;

  volatile uint32_t *dst = (void *)(intptr_t)(sector * 4096 + offset);
  const uint32_t *src = data;

  reg_wr(NVMC_CONFIG, 1);
  for(size_t i = 0; i < len / 4; i++) {
    *dst++ = *src++;
    flash_wait_ready();
  }
  reg_wr(NVMC_CONFIG, 0);
  return 0;
}


static const void *
flash_get_addr(const struct flash_iface *fif, int sector)
{
  return (void *)(intptr_t)(sector * 4096);
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


static void  __attribute__((section("ramcode"),noinline,noreturn))
flash_multi_write0(const flash_multi_write_chunks_t *chunks,
                   const void *src_base,
                   int flags)
{
  const size_t flashsize = reg_rd(0x10000110); // in kB
  const size_t num_sectors = flashsize / 4;


  reg_wr(NVMC_CONFIG, 2);

  for(size_t i = 0; i < num_sectors; i++) {

    const size_t sec_start = i * 4096;
    const size_t sec_end = sec_start + 4095;

    int erase = 0;

    for(int j = 0; j < chunks->num_chunks; j++) {
      const flash_multi_write_chunk_t *c = &chunks->chunks[j];
      const size_t start = c->dst_offset;
      const size_t end = start + (c->length - 1);
      if(end >= sec_start && start <= sec_end)
        erase = 1;
    }

    if(erase) {
      reg_wr(NVMC_ERASEPAGE, sec_start);
      flash_wait_ready();
    }
  }

  reg_wr(NVMC_CONFIG, 1);

  for(int i = 0; i < chunks->num_chunks; i++) {
    const flash_multi_write_chunk_t *c = &chunks->chunks[i];
    const uint32_t *src = c->src_offset + src_base;
    const size_t len = c->length / 4;
    volatile uint32_t *dst = (uint32_t *)c->dst_offset;

    for(size_t j = 0; j < len; j++) {
      if(dst[j] != 0xffffffff)
        asm volatile("bkpt 8");
      dst[j] = src[j];
      flash_wait_ready();
    }
  }

  static volatile uint32_t *const AIRCR = (volatile uint32_t *)0xe000ed0c;
  *AIRCR = 0x05fa0004;
  while(1) {}
}



static void
flash_multi_write(const struct flash_iface *fif,
                  const flash_multi_write_chunks_t *chunks,
                  const void *src_base,
                  int flags)
{
  int q = irq_forbid(IRQ_LEVEL_ALL);
  flash_multi_write0(chunks, src_base, flags);
  irq_permit(q);
}


static flash_sector_type_t
flash_get_sector_type(const struct flash_iface *fif, int sector)
{
  const size_t size = get_sector_size(sector);
  if(size == 0)
    return 0;

  const size_t offset = get_sector_offset(sector);

  extern unsigned long _sdata;
  extern unsigned long _etext;
  extern unsigned long _edata;
  const size_t data_size = (void *)&_edata - (void *)&_sdata;
  void *end_of_prog = (void *)&_etext + data_size;

  void *begin_of_sector = (void *)offset;

  if(end_of_prog > begin_of_sector)
    return FLASH_SECTOR_TYPE_PROG;

  return FLASH_SECTOR_TYPE_AVAIL;
}

static const flash_iface_t nrf52_flash = {
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
  return &nrf52_flash;
}

