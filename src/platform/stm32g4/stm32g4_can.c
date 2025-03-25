#include "stm32g4_can.h"

#include "stm32g4_reg.h"
#include "stm32g4_clk.h"

#define FDCAN_CREL   0x000
#define FDCAN_DBTP   0x00c
#define FDCAN_TEST   0x010
#define FDCAN_CCCR   0x018
#define FDCAN_NBTP   0x01c
#define FDCAN_ECR    0x040
#define FDCAN_PSR    0x044
#define FDCAN_IR     0x050
#define FDCAN_IE     0x054
#define FDCAN_ILS    0x058
#define FDCAN_ILE    0x05c

#define FDCAN_RXF0S  0x090
#define FDCAN_RXF0A  0x094
#define FDCAN_RXF1S  0x098
#define FDCAN_RXF1A  0x09c

#define FDCAN_TXFQS  0x0c4

#define FDCAN_TXBC   0x0c0
#define FDCAN_TXBAR  0x0cc

#define FDCAN_CKDIV  0x100

#include "platform/stm32/stm32_fdcan.c"

#define FDCAN_BASE(x) (0x40006000 + ((x) * 0x400))
#define FDCAN_RAM(x)  (0x4000a000 + ((x) * 0x400))

void
stm32g4_fdcan_init(int instance, gpio_t can_tx, gpio_t can_rx,
                   unsigned int nominal_bitrate,
                   unsigned int data_bitrate,
                   const struct dsig_filter *output_filter)
{
  if(instance != 1)
    panic("stm32g4_can: Only instance 1 is supported now");

  clk_enable(CLK_FDCAN);

  gpio_conf_af(can_tx, 9, GPIO_PUSH_PULL, GPIO_SPEED_LOW, GPIO_PULL_NONE);
  gpio_conf_af(can_rx, 9, GPIO_PUSH_PULL, GPIO_SPEED_LOW, GPIO_PULL_NONE);

  fdcan_t *fc = calloc(1, sizeof(fdcan_t));
  fc->reg_base = FDCAN_BASE(instance);
  fc->ram_base = FDCAN_RAM(instance);

  stm32_fdcan_cce(fc, NULL);

  for(size_t i = 0; i < 0x350; i += 4) {
    reg_wr(fc->ram_base + i, 0);
  }


  const char *name = "can";

  error_t err = stm32_fdcan_init(fc, name,
                                 nominal_bitrate,
                                 data_bitrate, clk_get_freq(CLK_FDCAN),
                                 NULL,
                                 output_filter);
  if(err) {
    printf("%s: Failed to initialize\n", name);
    return;
  }

  irq_enable_fn_arg(21, IRQ_LEVEL_NET, stm32_fdcan_irq0, fc);
  irq_enable_fn_arg(22, IRQ_LEVEL_NET, stm32_fdcan_irq1, fc);

  printf("%s: Initialized. Nominal bitrate:%d Data bitrate:%d\n",
         name, nominal_bitrate, data_bitrate);
}
