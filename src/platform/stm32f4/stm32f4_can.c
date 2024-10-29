#include "stm32f4_can.h"
#include "stm32f4_clk.h"


#define CAN_BASE(x) (0x40006000 + (x) * 0x400)

#include "platform/stm32/stm32_bxcan.c"

void
stm32f4_can_init(int instance, gpio_t txd, gpio_t rxd, int bitrate,
                 const struct dsig_filter *input_filter,
                 const struct dsig_filter *output_filter)
{
  gpio_conf_af(txd, 9, GPIO_PUSH_PULL, GPIO_SPEED_LOW, GPIO_PULL_NONE);
  gpio_conf_af(rxd, 9, GPIO_PUSH_PULL, GPIO_SPEED_LOW, GPIO_PULL_NONE);

  int irq_base = (instance - 1) * 44 + 19;
  stm32_bxcan_init(instance, bitrate, irq_base,
                   input_filter, output_filter);
}
