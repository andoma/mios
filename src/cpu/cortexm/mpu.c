#include "cpu.h"
#include "cache.h"

#include <mios/mios.h>
#include <stdio.h>

static uint8_t mpu_regions_used;
uint8_t redzone_rbar_bits;

static volatile uint32_t * const MPU_TYPE = (uint32_t *)0xe000ed90;
static volatile uint32_t * const MPU_CTRL = (uint32_t *)0xe000ed94;
static volatile uint32_t * const MPU_RBAR = (uint32_t *)0xe000ed9c;
static volatile uint32_t * const MPU_RASR = (uint32_t *)0xe000eda0;


void
mpu_disable(void)
{
  *MPU_CTRL = 0;
}

static void __attribute__((constructor(151)))
mpu_init(void)
{
  if(*MPU_TYPE & 1)
    panic("MPU: Separate regions not supported");

  int num_regions = (*MPU_TYPE >> 8) & 0xff;
  redzone_rbar_bits = 0x10 | (num_regions - 1);

  printf("MPU: %d regions\n", num_regions);

  if(CPU_STACK_REDZONE_SIZE == 32) {
    // Last MPU region is used as 32 byte stack redzone

    void *idle_stack = ((thread_t *)(curcpu()->sched.idle))->t_sp_bottom;
    *MPU_RBAR = (intptr_t)idle_stack | redzone_rbar_bits;
    *MPU_RASR = (4 << 1) | 1; // 2^(4 + 1) = 32 byte + enable
  }

  *MPU_CTRL = 5; // Enable MPU
}


int
mpu_set_region(void *ptr, int size_power_of_two, uint32_t flags, int region)
{
  if(size_power_of_two < 5)
    panic("mpu sizebits %d too small", size_power_of_two);

  size_power_of_two--;

  uint32_t rbar = (intptr_t)ptr | 0x10 | region;
  uint32_t rasr =
    flags |
    (size_power_of_two << 1) | // size
    1; // enable

  *MPU_RBAR = rbar;
  *MPU_RASR = rasr;
  return region;
}


int
mpu_add_region(void *ptr, int size_power_of_two, uint32_t flags)
{
  int r = mpu_regions_used++;
  return mpu_set_region(ptr, size_power_of_two, flags, r);
}


__attribute__((weak))
void
mpu_protect_code(int on)
{

}
