#include "tegra234-spe_can.h"

#include <mios/mios.h>

#include "reg.h"

#define FDCAN_CREL   0x000
#define FDCAN_DBTP   0x00c
#define FDCAN_TEST   0x010
#define FDCAN_CCCR   0x018
#define FDCAN_NBTP   0x01c
#define FDCAN_TSCC   0x020
#define FDCAN_TSCV   0x024
#define FDCAN_ECR    0x040
#define FDCAN_PSR    0x044
#define FDCAN_IR     0x050
#define FDCAN_IE     0x054
#define FDCAN_ILS    0x058
#define FDCAN_ILE    0x05c

#define FDCAN_SIDFC  0x084

#define FDCAN_RXF0C  0x0a0
#define FDCAN_RXF0S  0x0a4
#define FDCAN_RXF0A  0x0a8

#define FDCAN_RXF1C  0x0b0
#define FDCAN_RXF1S  0x0b4
#define FDCAN_RXF1A  0x0b8
#define FDCAN_RXESC  0x0bc

#define FDCAN_TXBC   0x0c0
#define FDCAN_TXFQS  0x0c4
#define FDCAN_TXESC  0x0c8

#define FDCAN_TXBAR  0x0d0

#include "platform/stm32/stm32_fdcan.c"


can_netif_t *
tegra243_spe_can_init(unsigned int nominal_bitrate,
                      unsigned int data_bitrate,
                      const struct dsig_filter *input_filter,
                      const struct dsig_filter *output_filter,
                      uint32_t flags, const char *name)
{
  reg_wr(0x20b13004, 0x00000013);
  reg_wr(0x20b10000, 0);
  reg_wr(0x0c311014, 1 << 3);

  fdcan_t *fc = calloc(1, sizeof(fdcan_t));
  fc->reg_base = 0x0c310000;

  uint32_t ram_offset = 0;
  fc->ram_base = 0x0c312000;

  stm32_fdcan_cce(fc, input_filter);

  for(size_t i = 0; i < 4096 / 4; i++) {
    reg_wr(fc->ram_base + i * 4, 0);
  }

  reg_wr(fc->reg_base + FDCAN_SIDFC,
         (ram_offset + FDCAN_FLSSA(0)) |
         (fc->num_std_filters << 16));

  reg_wr(fc->reg_base + FDCAN_TXBC,
         (ram_offset + FDCAN_TXBUF(0, 0)) |
         (0 << 16) |
         (3 << 24));

  reg_wr(fc->reg_base + FDCAN_TXESC, 7); // 64 byte data field

  reg_wr(fc->reg_base + FDCAN_RXF0C,
         (ram_offset + FDCAN_RXFIFO0(0, 0)) |
         (3 << 16) |
         0);

  reg_wr(fc->reg_base + FDCAN_RXF1C,
         (ram_offset + FDCAN_RXFIFO1(0, 0)) |
         (3 << 16) |
         0);

  reg_wr(fc->reg_base + FDCAN_RXESC,
         (7 << 0) |
         (7 << 4) |
         0);

  error_t err = stm32_fdcan_init(fc, name,
                                 nominal_bitrate, data_bitrate,
                                 50000000,
                                 input_filter,
                                 output_filter);
  if(err) {
    printf("%s: Failed to initialize\n", name);
    return NULL;
  }

  irq_enable_fn_arg(IRQ_CAN1_0, IRQ_LEVEL_NET, stm32_fdcan_irq0, fc);
  irq_enable_fn_arg(IRQ_CAN1_1, IRQ_LEVEL_HIGH, stm32_fdcan_irq1, fc);

  printf("%s: Initialized. Nominal bitrate:%d Data bitrate:%d\n",
         name, nominal_bitrate, data_bitrate);
  return &fc->cni;
}
