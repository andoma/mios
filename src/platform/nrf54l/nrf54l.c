#include <stdio.h>
#include <malloc.h>

#include "nrf54l_reg.h"

// FICR (Factory information) and its trim table
#define FICR_BASE        0x00ffc000
#define FICR_TRIMCNF     (FICR_BASE + 0x400) // 64 x {ADDR, DATA}
#define FICR_TRIMCNF_CNT 64

// OSCILLATORS (secure)
#define OSC_BASE         0x50120000
#define OSC_PLL_FREQ     (OSC_BASE + 0x800)
#define OSC_PLL_FREQ_CK128M 1
#define OSC_PLL_FREQ_CK64M  3

// RAM is 256 kB, contiguous from 0x20000000
#define RAM_END          0x20040000


// Replicate the parts of the MDK SystemInit() that the hardware needs to
// operate correctly: copy the factory trim values into their target
// registers (analog/oscillator trimming) and select the CPU frequency.
static void __attribute__((constructor(101)))
nrf54l_soc_init(void)
{
  // Copy trim values from FICR into the target registers. The list is
  // terminated by an ADDR of 0x00000000 or 0xffffffff.
  for(int i = 0; i < FICR_TRIMCNF_CNT; i++) {
    uint32_t addr = reg_rd(FICR_TRIMCNF + i * 8);
    if(addr == 0x00000000 || addr == 0xffffffff)
      break;
    uint32_t data = reg_rd(FICR_TRIMCNF + i * 8 + 4);
    reg_wr(addr, data);
  }

  // Engineering-sample device configuration (from SystemInit)
  if(reg_rd(0x50120440) == 0)
    reg_wr(0x50120440, 0xc8);

  // Set MCU power domain (CPU) clock to 128 MHz
  reg_wr(OSC_PLL_FREQ, OSC_PLL_FREQ_CK128M);
}


static void __attribute__((constructor(102)))
nrf54l_init_heap(void)
{
  heap_add_mem(HEAP_START_EBSS, RAM_END,
               MEM_TYPE_DMA | MEM_TYPE_VECTOR_TABLE | MEM_TYPE_CODE, 10);
}


static void __attribute__((constructor(120)))
nrf54l_banner(void)
{
  printf("\nnRF54L15 (1524 kB RRAM, 256 kB RAM)\n");
}
