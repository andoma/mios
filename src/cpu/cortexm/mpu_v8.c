#include "cpu.h"
#include "cache.h"

#include <mios/mios.h>
#include <stdio.h>

// PMSAv8-M MPU registers

static volatile uint32_t * const MPU_TYPE  = (uint32_t *)0xe000ed90;
static volatile uint32_t * const MPU_CTRL  = (uint32_t *)0xe000ed94;
static volatile uint32_t * const MPU_RNR   = (uint32_t *)0xe000ed98;
static volatile uint32_t * const MPU_RBAR  = (uint32_t *)0xe000ed9c;
static volatile uint32_t * const MPU_RLAR  = (uint32_t *)0xe000eda0;
static volatile uint32_t * const MPU_MAIR0 = (uint32_t *)0xe000edc0;
static volatile uint32_t * const MPU_MAIR1 = (uint32_t *)0xe000edc4;

static uint8_t mpu_regions_used;

void
mpu_disable(void)
{
  *MPU_CTRL = 0;
  asm volatile ("dsb");
  asm volatile ("isb");
}


static void
mpu_set_mair(void)
{
  // Attr0: Device-nGnRnE  (0x00)
  // Attr1: Normal, Non-cacheable (0x44)
  // Attr2: Normal, Write-Through (0xaa)
  // Attr3: Normal, Write-Back (0xff)
  *MPU_MAIR0 = 0xffaa4400;
  *MPU_MAIR1 = 0;
}


static void __attribute__((constructor(101)))
mpu_init(void)
{
  int num_regions = (*MPU_TYPE >> 8) & 0xff;
  printf("MPU: %d regions (PMSAv8)\n", num_regions);

  mpu_set_mair();
}


void
mpu_setup_region(int region, uint32_t base, uint32_t limit,
                 uint32_t rbar_flags, uint32_t rlar_flags)
{
  *MPU_RNR  = region;
  *MPU_RBAR = (base & ~0x1f) | rbar_flags;
  *MPU_RLAR = (limit & ~0x1f) | rlar_flags | 1; // EN=1
}


int
mpu_add_region_v8(uint32_t base, uint32_t limit,
                  uint32_t rbar_flags, uint32_t rlar_flags)
{
  int r = mpu_regions_used++;
  mpu_setup_region(r, base, limit, rbar_flags, rlar_flags);
  return r;
}


void
mpu_enable(void)
{
  asm volatile ("dsb");
  asm volatile ("isb");
  *MPU_CTRL = 5; // ENABLE + PRIVDEFENA (privileged default map as background)
  asm volatile ("dsb");
  asm volatile ("isb");
}


__attribute__((weak))
void
mpu_protect_code(int on)
{

}
