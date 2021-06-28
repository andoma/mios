#include <stdlib.h>
#include <unistd.h>
#include <mios/mios.h>
#include <mios/flash.h>

#include "stm32f4_reg.h"
#include "stm32f4_flash.h"

#include "systick.h"
#include "irq.h"


typedef struct {
  uint16_t offset; // in kB
  uint16_t size;   // in kB
} sector_t;


static const sector_t sectors[12] = {
  { 0,    16 },
  { 16,   16 },
  { 32,   16 },
  { 48,   16 },
  { 64,   64 },
  { 128,  128 },
  { 256,  128 },
  { 384,  128 },
  { 512,  128 },
  { 640,  128 },
  { 768,  128 },
  { 896,  128 },
};


static const sector_t *
get_sector(unsigned int sector)
{
  if(sector >= ARRAYSIZE(sectors))
    return NULL;
  return &sectors[sector];
}





#define FLASH_BASE 0x40023c00

#define FLASH_KEYR    (FLASH_BASE + 0x04)
#define FLASH_IOTKEYR (FLASH_BASE + 0x08)
#define FLASH_SR      (FLASH_BASE + 0x0c)
#define FLASH_CR      (FLASH_BASE + 0x10)

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
  const sector_t *s = get_sector(sector);
  if(s == NULL)
    return 0;
  return s->size * 1024;
}


static void __attribute__((section("ramcode")))
flash_erase_sector0(int sector)
{
  reg_wr(FLASH_CR, 0x2 | (sector << 3));
  reg_set_bit(FLASH_CR, 16);
  flash_wait_ready();
}


static error_t
flash_erase_sector(const struct flash_iface *fif, int sector)
{
  const sector_t *s = get_sector(sector);
  if(s == NULL)
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
  const sector_t *s = get_sector(sector);
  if(s == NULL)
    return ERR_INVALID_ID;

  if(offset + len >= s->size * 1024)
    return ERR_INVALID_ADDRESS;

  const uint8_t *d8 = data;
  volatile uint8_t *addr = (uint8_t *)(0x8000000 + offset + s->offset * 1024);

  flash_unlock();

  while(reg_rd(FLASH_SR) & (1 << 16)) {
  }

  reg_wr(FLASH_CR, 0x1 | (0 << 8));

  for(size_t i = 0; i < len; i++)
    addr[i] = d8[i];

  flash_lock();
  return 0;
}


static error_t
flash_read(const struct flash_iface *fif, int sector,
           size_t offset, void *data, size_t len)
{
  const sector_t *s = get_sector(sector);
  if(s == NULL)
    return ERR_INVALID_ID;

  if(offset + len >= s->size * 1024)
    return ERR_INVALID_ADDRESS;

  uint8_t *d8 = data;
  volatile uint8_t *addr = (uint8_t *)(0x8000000 + offset + s->offset * 1024);

  for(size_t i = 0; i < len; i++)
    d8[i] = addr[i];

  return 0;
}


static error_t
flash_compare(const struct flash_iface *fif, int sector,
              size_t offset, const void *data, size_t len)
{
  const sector_t *s = get_sector(sector);
  if(s == NULL)
    return ERR_INVALID_ID;

  if(offset + len >= s->size * 1024)
    return ERR_INVALID_ADDRESS;

  const uint8_t *d8 = data;
  volatile uint8_t *addr = (uint8_t *)(0x8000000 + offset + s->offset * 1024);

  for(size_t i = 0; i < len; i++) {
    if(d8[i] != addr[i])
      return ERR_MISMATCH;
  }

  return 0;
}



static const flash_iface_t stm32f4_flash = {
  .get_sector_size = flash_get_sector_size,
  .erase_sector = flash_erase_sector,
  .write = flash_write,
  .read = flash_read,
  .compare = flash_compare,
};


const flash_iface_t *
stm32f4_flash_get(void)
{
  return &stm32f4_flash;
}
