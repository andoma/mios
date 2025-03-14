#include "stm32h7_can.h"

#include <mios/mios.h>

#include "stm32h7_reg.h"
#include "stm32h7_clk.h"

#define FDCAN_CREL   0x000
#define FDCAN_DBTP   0x00c
#define FDCAN_TEST   0x010
#define FDCAN_CCCR   0x018
#define FDCAN_NBTP   0x01c
#define FDCAN_ECR    0x040
#define FDCAN_PSR    0x044
#define FDCAN_IR     0x050
#define FDCAN_IE     0x054
#define FDCAN_IKE    0x05c

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

static const struct {
  uint32_t reg_base;
  uint8_t irq0;
  uint8_t irq1;
  char name[4];
} stm32h7_can_interfaces[] = {
  { 0x4000a000, 19,  21,  "can1" },
  { 0x4000a400, 20,  22,  "can2" },
  { 0x4000d400, 159, 160, "can3" },
};

// 10240 bytes of shared CAN RAM
#define CAN_RAM 0x4000ac00



void
stm32h7_can_init(int instance, gpio_t can_tx, gpio_t can_rx,
                 unsigned int nominal_bitrate,
                 unsigned int data_bitrate,
                 const struct dsig_filter *output_filter)
{
  instance--;
  if(instance >= ARRAYSIZE(stm32h7_can_interfaces))
    panic("Invalid CAN interface %u", instance + 1);

  gpio_conf_af(can_tx, 9, GPIO_PUSH_PULL, GPIO_SPEED_LOW, GPIO_PULL_NONE);
  gpio_conf_af(can_rx, 9, GPIO_PUSH_PULL, GPIO_SPEED_LOW, GPIO_PULL_NONE);

  clk_enable(CLK_FDCAN);

  fdcan_t *fc = calloc(1, sizeof(fdcan_t));
  fc->reg_base = stm32h7_can_interfaces[instance].reg_base;
  fc->ram_base = CAN_RAM;

  stm32_fdcan_cce(fc);

  for(size_t i = 0; i < 10240 / 4; i++) {
    reg_wr(fc->ram_base + i * 4, 0);
  }

  reg_wr(fc->reg_base + FDCAN_TXBC,
         FDCAN_TXBUF(0, 0) |
         (0 << 16) |
         (3 << 24));

  reg_wr(fc->reg_base + FDCAN_TXESC, 7); // 64 byte data field

  reg_wr(fc->reg_base + FDCAN_RXF0C,
         FDCAN_RXFIFO0(0, 0) |
         (3 << 16) |
         0);

  reg_wr(fc->reg_base + FDCAN_RXF1C,
         FDCAN_RXFIFO1(0, 0) |
         (3 << 16) |
         0);

  reg_wr(fc->reg_base + FDCAN_RXESC,
         (7 << 0) |
         (7 << 4) |
         0);


  const char *name = stm32h7_can_interfaces[instance].name;
  error_t err = stm32_fdcan_init(fc, name,
                                 nominal_bitrate, data_bitrate,
                                 clk_get_freq(CLK_FDCAN),
                                 output_filter);
  if(err) {
    printf("%s: Failed to initialize\n", name);
    return;
  }

  irq_enable_fn_arg(stm32h7_can_interfaces[instance].irq0,
                    IRQ_LEVEL_NET, stm32_fdcan_irq, fc);

  printf("%s: Initialized. Nominal bitrate:%d Data bitrate:%d\n",
         name, nominal_bitrate, data_bitrate);
}
