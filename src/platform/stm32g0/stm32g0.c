#include <stdint.h>
#include <stdio.h>
#include <malloc.h>

static volatile uint16_t *const FLASH_SIZE = (volatile uint16_t *)0x1fff75e0;
static volatile uint32_t *const DBG_IDCODE = (volatile uint32_t *)0x40015800;

static struct {
  uint16_t id;
  uint16_t ramsize;
  const char name[4];
} devicemap[] = {
  { 0x466, 8, "3x" },
  { 0x460, 32, "7x" },
};


static void  __attribute__((constructor(120)))
stm32g0_init(void)
{
  extern unsigned long _ebss;

  uint32_t chipid = *DBG_IDCODE;
  uint16_t deviceid = chipid & 0xfff;

  int ramsize = 8;
  const char *name = "???";
  for(size_t i = 0; i < sizeof(devicemap) / sizeof(devicemap[0]); i++) {
    if(devicemap[i].id == deviceid) {
      ramsize = devicemap[i].ramsize;
      name = devicemap[i].name;
      break;
    }
  }

  printf("\nSTM32G0%s (%d kB Flash) (%d kB RAM) ID:0x%08x\n",
         name, *FLASH_SIZE, ramsize, chipid);

  void *SRAM1_start = (void *)&_ebss;
  void *SRAM1_end   = (void *)0x20000000 + ramsize * 1024;

  // SRAM1
  heap_add_mem((long)SRAM1_start, (long)SRAM1_end,
               MEM_TYPE_DMA | MEM_TYPE_VECTOR_TABLE | MEM_TYPE_CODE, 10);

}
