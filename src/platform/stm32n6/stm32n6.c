#include "stm32n6_clk.h"
#include "stm32n6_ramcfg.h"
#include "stm32n6_rif.h"
#include "stm32n6_syscfg.h"
#include "stm32n6_bootstatus.h"

#include <malloc.h>

#include <mios/atomic.h>
#include <mios/sys.h>

#include <net/pbuf.h>

#include "mpu_v8.h"

#include <stdio.h>
#include <mios/eventlog.h>

// AXISRAM layout (nonsecure aliases):
//   AXISRAM1:  0x24000000 - 0x240FFFFF  (1 MB)   - CPU_NOC, DMA accessible
//   AXISRAM2:  0x24100000 - 0x241FFFFF  (1 MB)   - CPU_NOC, DMA accessible
//   AXISRAM3:  0x24200000 - 0x2426FFFF  (448 KB) - NPU_NIC
//   AXISRAM4:  0x24270000 - 0x242DFFFF  (448 KB) - NPU_NIC
//   AXISRAM5:  0x242E0000 - 0x2434FFFF  (448 KB) - NPU_NIC
//   AXISRAM6:  0x24350000 - 0x243BFFFF  (448 KB) - NPU_NIC
//
// DMA carveouts from top of AXISRAM2 (growing downward):

#define AXISRAM2_END      0x24200000
#define AXISRAM36_END     0x243C0000

#define NOCACHE_SIZE      0x1000      // 4 KB non-cached DMA descriptors
#define PBUF_ARENA_SIZE   0x10000     // 64 KB pbuf data arena

#define NOCACHE_END       AXISRAM2_END
#define NOCACHE_BASE      (NOCACHE_END - NOCACHE_SIZE)
#define PBUF_ARENA_END    NOCACHE_BASE
#define PBUF_ARENA_BASE   (PBUF_ARENA_END - PBUF_ARENA_SIZE)

#define HEAP_DMA_END      PBUF_ARENA_BASE

static volatile uint32_t *const DWT_CONTROL  = (volatile uint32_t *)0xE0001000;

// ITCM baseline is 64 KB at 0x00000000 (RM0486 §2.3 memory map). The first
// 4 KB are deliberately left unmapped — combined with PRIVDEFENA=0 below,
// any access there (read, write or fetch) traps. The remaining 60 KB hold
// the .fastcode segment (linker-placed, see stm32n6.ld) at the bottom and
// the runtime ITCM heap on top. The whole 60 KB is one MPU region, mapped
// RO by default and toggled to RW by mpu_protect_code() while code /
// vector-table writes are in flight.
#define ITCM_BASE       0x00000000
#define ITCM_GUARD_END  0x00001000
#define ITCM_END        0x00010000

extern char _efastcode[];

static int code_itcm_region;

static void  __attribute__((constructor(120)))
stm32n6_init(void)
{
  *DWT_CONTROL = 1;


  printf("\nSTM32N6\n");

  // Enable interleaving for AXISRAM3-6
  reg_set_bit(SYSCFG_NPU_ICNCR, 0);

  // Enable clocks for AXISRAM3-6
  reg_or(RCC_MEMENR, (1 << 0) | (1 << 1) | (1 << 2) | (1 << 3) | (1 << 8));

  // Turn off shutdown flag for AXISRAM3-6
  for(int i = 3; i <= 6; i++) {
    reg_clr_bit(RAMCFG_AXISRAMxCR(i), 20);
  }
  asm volatile("dsb");

  // AXISRAM1+2 DMA heap (below carveouts)
  heap_add_mem(HEAP_START_EBSS, HEAP_DMA_END,
               MEM_TYPE_DMA | MEM_TYPE_VECTOR_TABLE | MEM_TYPE_CODE, 20);

  // AXISRAM3-6 general heap (not DMA accessible)
  heap_add_mem(AXISRAM2_END, AXISRAM36_END, 0, 15);

  // DTCM (128 KB, minus 1 KB for MSP stack)
  heap_add_mem(0x20000400, 0x20020000, MEM_TYPE_LOCAL, 30);

  // Non-cached region for DMA descriptor rings (top of AXISRAM2)
  // heap_add_mem before MPU region so writes go through cache normally,
  // then MPU makes it non-cached for subsequent DMA use
  heap_add_mem(NOCACHE_BASE, NOCACHE_END,
               MEM_TYPE_DMA | MEM_TYPE_NO_CACHE, 40);

  // ITCM heap above the linker-placed .fastcode segment. The bootloader
  // pre-scrubs ITCM ECC so heap metadata writes don't fault.
  heap_add_mem((long)_efastcode, ITCM_END,
               MEM_TYPE_CODE | MEM_TYPE_VECTOR_TABLE, 50);

  // We disable PRIVDEFENA below, so every range we expect to access has
  // to be mapped here. The first 4 KB of ITCM is intentionally omitted
  // so null-pointer reads/writes/execution all fault.
  code_itcm_region =
    mpu_add_region_v8(ITCM_GUARD_END, ITCM_END,
                      MPU_V8_AP_RO | MPU_V8_SH_NONE,
                      MPU_V8_ATTR_NORMAL_WB);

  // DTCM (covers MSP stack at 0x20000000-0x200003FF and the local heap)
  mpu_add_region_v8(0x20000000, 0x20020000,
                    MPU_V8_XN | MPU_V8_AP_RW | MPU_V8_SH_NONE,
                    MPU_V8_ATTR_NORMAL_WB);

  // AXISRAM1+2 main: vector table, code, data, BSS, heap, pbuf arena
  mpu_add_region_v8(0x24000000, NOCACHE_BASE,
                    MPU_V8_AP_RW | MPU_V8_SH_NONE,
                    MPU_V8_ATTR_NORMAL_WB);

  // AXISRAM1+2 NOCACHE carveout (DMA descriptor rings)
  mpu_add_region_v8(NOCACHE_BASE, NOCACHE_END,
                    MPU_V8_XN | MPU_V8_AP_RW | MPU_V8_SH_NONE,
                    MPU_V8_ATTR_NORMAL_NC);

  // AXISRAM3-6 (general heap, not DMA accessible)
  mpu_add_region_v8(AXISRAM2_END, AXISRAM36_END,
                    MPU_V8_XN | MPU_V8_AP_RW | MPU_V8_SH_NONE,
                    MPU_V8_ATTR_NORMAL_WB);

  // Peripherals: 0x40000000-0x5FFFFFFF (covers AHB/APB peripherals,
  // RIF, BSEC scratch, RAMCFG, SYSCFG, RCC, IWDG, etc.)
  mpu_add_region_v8(0x40000000, 0x60000000,
                    MPU_V8_XN | MPU_V8_AP_RW | MPU_V8_SH_NONE,
                    MPU_V8_ATTR_DEVICE);

  // Enable MPU without PRIVDEFENA — anything outside the regions above
  // (notably the first 4 KB of ITCM) faults on access.
  asm volatile("dsb;isb");
  *(volatile uint32_t *)0xE000ED94 = 1; // ENABLE only
  asm volatile("dsb;isb");

  // Pbuf data arena (cache-line aligned, used by ethernet DMA)
  pbuf_data_add((void *)PBUF_ARENA_BASE, (void *)PBUF_ARENA_END);

  // Configure ETH1 DMA master as CID 1, Secure, Privileged
  stm32n6_rif_set_master_attr(RIF_MASTER_ETH1, 1, 1, 1);

  // Configure ETH1 peripheral as Secure + Privileged
  stm32n6_rif_set_periph_sec(RIF_PERIPH_ETH1_REG, RIF_PERIPH_ETH1_BIT, 1, 1);

  // Open RISAF2 (AXISRAM1/2) for all CIDs
  stm32n6_risaf_open_base_region(RISAF2_BASE, 0xFF);
}


static atomic_t code_unprotect_counter;

void
mpu_protect_code(int on)
{
  int n = atomic_add_and_fetch(&code_unprotect_counter, on ? -1 : 1);
  uint32_t ap = n ? MPU_V8_AP_RW : MPU_V8_AP_RO;
  mpu_setup_region(code_itcm_region, ITCM_GUARD_END, ITCM_END,
                   ap | MPU_V8_SH_NONE,
                   MPU_V8_ATTR_NORMAL_WB);
  asm volatile("dsb;isb");
}


const struct serial_number
sys_get_serial_number(void)
{
  struct serial_number sn;
  sn.data = (const void *)0x46009014;
  sn.len = 12;
  return sn;
}


// Confirm successful boot — clear dirty bit for current slot.
// Only act if FSBL set the FSBL_RAN flag (skip when loaded via GDB).
// Priority 9999 = just before main(), after all platform init.
static void __attribute__((constructor(9999)))
boot_confirm(void)
{
  uint32_t bs = reg_rd(BSEC_SCRATCH0);
  if(!(bs & BOOTSTATUS_FSBL_RAN))
    return;

  // Clear dirty bit for current slot
  if(bs & BOOTSTATUS_BOOTED_B)
    bs &= ~BOOTSTATUS_APP_B_DIRTY;
  else
    bs &= ~BOOTSTATUS_APP_A_DIRTY;

  // Clear FSBL_RAN so it must be set again on next FSBL boot
  bs &= ~BOOTSTATUS_FSBL_RAN;

  reg_wr(BSEC_SCRATCH0, bs);

  evlog(LOG_INFO, "Booted FSBL:%c App:%c (A:%s B:%s)",
        (bs & BOOTSTATUS_FSBL_SLOT_B) ? 'B' : 'A',
        (bs & BOOTSTATUS_BOOTED_B) ? 'B' : 'A',
        (bs & BOOTSTATUS_APP_A_DIRTY) ? "dirty" : "ok",
        (bs & BOOTSTATUS_APP_B_DIRTY) ? "dirty" : "ok");
}
