#include <unistd.h>
#include <assert.h>
#include <stdio.h>
#include <mios/io.h>
#include <mios/mios.h>
#include <mios/task.h>

#include "irq.h"

#include <mios/dsig.h>

#include "stm32g4_reg.h"
#include "stm32g4_clk.h"
#include "stm32g4_usb.h"
#include "stm32g4_can.h"

#include "platform/stm32/stm32_fdcan.h"

static void __attribute__((constructor(101)))
board_setup_early(void)
{
  stm32g4_init_pll(0, 60);
}


static void __attribute__((constructor(1000)))
board_setup_late(void)
{
  struct usb_interface_queue q;
  STAILQ_INIT(&q);

  usb_cdc_create_shell(&q);
  usb_dfu_runtime_create(&q);
  usb_mcp_create(&q, 0x01);

  stm32g4_usb_create(0x6666, 0x0010, "Lonelycoder", "stm32g4", &q);

  // FDCAN1 on PB9 (TX) / PB8 (RX) in external loopback: full frames
  // (with self-ACK) appear on the TX pin without any transceiver.
  static const struct dsig_filter can_out[] = {
    { .prefix = 0, .prefixlen = 0 },
    DSIG_FILTER_END
  };
  stm32g4_fdcan_init(1, GPIO_PB(9), GPIO_PB(8),
                     1000000, 1000000, can_out,
                     FDCAN_ENABLE_LOOPBACK);
}
