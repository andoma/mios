#include <unistd.h>
#include <assert.h>
#include <stdio.h>
#include <mios/io.h>
#include <mios/mios.h>
#include <mios/task.h>

#include "irq.h"

#include "stm32g4_reg.h"
#include "stm32g4_clk.h"
#include "stm32g4_usb.h"

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

  stm32g4_usb_create(0x6666, 0x0010, "Lonelycoder", "stm32g4", &q);
}
