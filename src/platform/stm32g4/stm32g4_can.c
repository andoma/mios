#include "stm32g4_can.h"
#include "stm32g4_clk.h"

#define FDCAN_BASE(x) (0x40006000 + ((x) * 0x400))
#define FDCAN_RAM(x)  (0x4000a000 + ((x) * 0x400))

#include "platform/stm32/stm32_fdcan.c"

void
stm32g4_fdcan_init(int instance, gpio_t can_tx, gpio_t can_rx,
                   const struct dsig_filter *output_filter)
{
  gpio_conf_af(can_tx, 9, GPIO_PUSH_PULL, GPIO_SPEED_LOW, GPIO_PULL_NONE);
  gpio_conf_af(can_rx, 9, GPIO_PUSH_PULL, GPIO_SPEED_LOW, GPIO_PULL_NONE);

  fc->reg_base = FDCAN_BASE(instance);
  fc->ram_base = FDCAN_RAM(instance);

  reg_set_bits(RCC_CCIPR, 24, 2, 2); // FDCAN clocked from PCLK
  clk_enable(CLK_FDCAN);

  stm32_fdcan_init(FDCAN_BASE(instance), FDCAN_RAM(instance),
                   21, output_filter);
}
