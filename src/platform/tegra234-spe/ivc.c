#include "tegra234_hsp.h"

#include "reg.h"

#include <stddef.h>
#include <mios/mios.h>

#include "tegra234_ast.h"


static void
ccplex_ivc_rx(void *arg)
{
  const uint32_t mbox = 5;

  uint32_t val = hsp_mbox_rd(NV_ADDRESS_MAP_TOP1_HSP_BASE, mbox) & 0x7fffffff;

  if(val == 0x2AAA5555) {

    // IVC Ready. Kernel sends us physical addr + size of shared memory
    // in two shared semaphores. A bit odd, but works I suppose?

    uint32_t carveout_base = hsp_ss_rd_and_clr(NV_ADDRESS_MAP_AON_HSP_BASE,
                                               IVC_SS_CARVEOUT_BASE);
    uint32_t carveout_size = hsp_ss_rd_and_clr(NV_ADDRESS_MAP_AON_HSP_BASE,
                                               IVC_SS_CARVEOUT_SIZE);

    ast_set_region(NV_ADDRESS_MAP_AON_AST_0_BASE,
                   1,
                   carveout_base,
                   0x80000000,
                   carveout_size,
                   1); // StreamID för AON
  } else {
    panic("IVC_RX Unknown mailbox operation: 0x%x", val);
  }
  hsp_mbox_wr(NV_ADDRESS_MAP_TOP1_HSP_BASE, mbox, 0); // Acknowledge
}

static void  __attribute__((constructor(1000)))
ivc_init_late(void)
{
  // RX
  uint32_t ie = hsp_connect_mbox(NV_ADDRESS_MAP_TOP1_HSP_BASE,
                                 ccplex_ivc_rx, NULL, HSP_MBOX_IRQ_FULL(5));

  reg_set_bit(ie, HSP_MBOX_IRQ_FULL(5));
}

