#include "stm32n6_clk.h"
#include "stm32n6_ramcfg.h"
#include "stm32n6_rif.h"
#include "stm32n6_syscfg.h"
#include "stm32n6_bootstatus.h"

#include <malloc.h>

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




static void  __attribute__((constructor(120)))
stm32n6_init(void)
{
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
  mpu_add_region_v8(NOCACHE_BASE, NOCACHE_END,
                    MPU_V8_XN | MPU_V8_AP_RW | MPU_V8_SH_NONE,
                    MPU_V8_ATTR_NORMAL_NC);

  mpu_enable();

  // Pbuf data arena (cache-line aligned, used by ethernet DMA)
  pbuf_data_add((void *)PBUF_ARENA_BASE, (void *)PBUF_ARENA_END);

  // Configure ETH1 DMA master as CID 1, Secure, Privileged
  stm32n6_rif_set_master_attr(RIF_MASTER_ETH1, 1, 1, 1);

  // Configure ETH1 peripheral as Secure + Privileged
  stm32n6_rif_set_periph_sec(RIF_PERIPH_ETH1_REG, RIF_PERIPH_ETH1_BIT, 1, 1);

  // Open RISAF2 (AXISRAM1/2) for all CIDs
  stm32n6_risaf_open_base_region(RISAF2_BASE, 0xFF);
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

  evlog(LOG_INFO, "Booted via application slot %c (A:%s B:%s)",
        (bs & BOOTSTATUS_BOOTED_B) ? 'B' : 'A',
        (bs & BOOTSTATUS_APP_A_DIRTY) ? "dirty" : "ok",
        (bs & BOOTSTATUS_APP_B_DIRTY) ? "dirty" : "ok");
}
