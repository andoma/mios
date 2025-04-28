#include "tegra234_hsp.h"

#include "tegra234_ast.h"

#include "reg.h"
#include "net/netif.h"

#include <stdio.h>
#include <stddef.h>
#include <mios/mios.h>
#include <mios/eventlog.h>


#define IVC_SS_CARVEOUT_BASE 0
#define IVC_SS_CARVEOUT_SIZE 1
#define IVC_SS_RX            2
#define IVC_SS_TX            3


struct tegra_ivc_channel {
  uint32_t write_count;
  uint32_t state;
  uint8_t pad1[56];
  uint32_t read_count;
  uint8_t pad2[60];
  uint8_t data[0];
};



static void
ivc_notify_cb(struct net_task *task, uint32_t signals)
{
  // Do we wait clearing the bits til after processing?

  uint32_t channels = hsp_ss_rd_and_clr(NV_ADDRESS_MAP_AON_HSP_BASE, IVC_SS_TX);
  printf("ivc notify %x\n", channels);

  if(channels & 0x2) {
    // ECHO
    struct tegra_ivc_channel *rx = (void *)0x80010100;
    size_t num_buffers = 16;

    while(rx->read_count != rx->write_count) {
      uint32_t index = rx->read_count & (num_buffers - 1);

      const void *payload = rx->data + index * 64;
      printf("%d: %s\n", rx->read_count, (const char *)payload);
      rx->read_count++;
    }

    printf("ack\n");
    if(hsp_ss_rd(NV_ADDRESS_MAP_AON_HSP_BASE, IVC_SS_RX)) {
      panic("sema not empty");
    }
    hsp_ss_set(NV_ADDRESS_MAP_AON_HSP_BASE, IVC_SS_RX, (1 << 1));
    const uint32_t mbox = 4;
    if(hsp_mbox_rd(NV_ADDRESS_MAP_TOP1_HSP_BASE, mbox))
      panic("mbox not empty");
    hsp_mbox_wr(NV_ADDRESS_MAP_TOP1_HSP_BASE, mbox, (1 << 31) | 0xaabb);
  }
}

static net_task_t ivc_mbox_task = { ivc_notify_cb };

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

    netlog("IVC ready base:0x%x size:0x%x", carveout_base,
          carveout_size);
    ast_set_region(NV_ADDRESS_MAP_AON_AST_0_BASE,
                   1,
                   carveout_base,
                   0x80000000,
                   carveout_size,
                   1); // StreamID för AON
  } else if(val == 0xaabb) {
    net_task_raise(&ivc_mbox_task, 1);
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

