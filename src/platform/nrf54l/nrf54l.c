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

// TAMPC (tamper controller) gates SWD/debug access. The DBGEN/NIDEN signals
// default to "disabled" after every reset, so unless we re-open them on each
// boot the part locks out the debugger the first time it resets (e.g. a
// watchdog bite while the core is halted), exactly as stock SystemInit does
// in a non-ENABLE_APPROTECT build. The LOCK bit, if set, means a previous
// session/hardware write-protected the signal and we must leave it alone.
#define TAMPC_BASE             0x500dc000
#define TAMPC_DOMAIN0_DBGEN    (TAMPC_BASE + 0x500) // PROTECT.DOMAIN[0].DBGEN.CTRL
#define TAMPC_DOMAIN0_NIDEN    (TAMPC_BASE + 0x508) // PROTECT.DOMAIN[0].NIDEN.CTRL
#define TAMPC_DOMAIN0_SPIDEN   (TAMPC_BASE + 0x510) // PROTECT.DOMAIN[0].SPIDEN.CTRL
#define TAMPC_DOMAIN0_SPNIDEN  (TAMPC_BASE + 0x518) // PROTECT.DOMAIN[0].SPNIDEN.CTRL
#define TAMPC_AP0_DBGEN        (TAMPC_BASE + 0x700) // PROTECT.AP[0].DBGEN.CTRL
#define TAMPC_LOCK             (1u << 1)            // CTRL.LOCK (1 = locked)
#define TAMPC_CLEAR_WP         0x50fa00f0           // KEY=0x50FA, WRITEPROTECTION=Clear
#define TAMPC_OPEN             0x50fa0001           // KEY=0x50FA, VALUE=High, LOCK=Disabled

static void
nrf54l_open_debug_signal(uint32_t ctrl)
{
  if(reg_rd(ctrl) & TAMPC_LOCK)
    return; // hardware/previous session locked it; can't reopen from here
  reg_wr(ctrl, TAMPC_CLEAR_WP);
  reg_wr(ctrl, TAMPC_OPEN);
}


// Replicate the parts of the MDK SystemInit() that the hardware needs to
// operate correctly: copy the factory trim values into their target
// registers (analog/oscillator trimming) and select the CPU frequency.
static void __attribute__((constructor(101)))
nrf54l_soc_init(void)
{
  // Keep SWD/debug available across resets (see TAMPC note above). Both the
  // non-secure (DBGEN/NIDEN) and secure (SPIDEN/SPNIDEN) signals must be
  // opened: OpenOCD reaches the core through the secure AP, so leaving the
  // secure signals locked still blocks the debugger after a reset.
  nrf54l_open_debug_signal(TAMPC_DOMAIN0_DBGEN);
  nrf54l_open_debug_signal(TAMPC_DOMAIN0_NIDEN);
  nrf54l_open_debug_signal(TAMPC_DOMAIN0_SPIDEN);
  nrf54l_open_debug_signal(TAMPC_DOMAIN0_SPNIDEN);
  nrf54l_open_debug_signal(TAMPC_AP0_DBGEN);

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
