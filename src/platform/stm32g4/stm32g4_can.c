#include "stm32g4_can.h"

// TODO: Move to common stm32 code

#include "stm32g4_clk.h"

#include "irq.h"

#include <stdio.h>
#include <stdlib.h>

#include "net/can/can.h"

#include <mios/bytestream.h>

#define FDCAN_BASE(x) (0x40006000 + ((x) * 0x400))
#define FDCAN_RAM(x)  (0x4000a000 + ((x) * 0x400))

#define FDCAN_CREL   0x000
#define FDCAN_TEST   0x010
#define FDCAN_CCCR   0x018
#define FDCAN_IR     0x050
#define FDCAN_IE     0x054
#define FDCAN_IKE    0x05c

#define FDCAN_RXF0S  0x090
#define FDCAN_RXF0A  0x094

#define FDCAN_TXFQS  0x0c4

#define FDCAN_TXBAR  0x0cc
#define FDCAN_CKDIV  0x100





#define FDCAN_FLSSA         0x000
#define FDCAN_FLESA         0x070
#define FDCAN_RXFIFO0(x,y) (0x0b0 + 72 * (x) + 4 * (y))
#define FDCAN_RXFIFO1(x,y) (0x188 + 72 * (x) + 4 * (y))
#define FDCAN_TXEVENT       0x260
#define FDCAN_TXBUF(x,y)   (0x278 + 72 * (x) + 4 * (y))


typedef struct fdcan {
  can_netif_t cni;

  uint32_t reg_base;
  uint32_t ram_base;

} fdcan_t;









static void
stm32g4_fdcan_irq(void *arg)
{
  fdcan_t *fc = arg;

  uint32_t bits = reg_rd(fc->reg_base + FDCAN_IR);
  reg_wr(fc->reg_base + FDCAN_IR, bits);

  if(bits & (1 << 0)) {
    // RX-Fifo 0 not empty

    uint32_t rstatus = reg_rd(fc->reg_base + FDCAN_RXF0S);
    uint32_t get_index = (rstatus >> 8) & 3;

    uint32_t w1 = reg_rd(fc->ram_base + FDCAN_RXFIFO0(get_index, 1));

    uint32_t len = (w1 >> 16) & 0xf;
    pbuf_t *pb = pbuf_make(0, 0);
    if(pb != NULL) {
      uint32_t w0 = reg_rd(fc->ram_base + FDCAN_RXFIFO0(get_index, 0));
      uint32_t *dst = pbuf_data(pb, 0);
      dst[0] = w0 & 0x1fffffff;
      for(int i = 0, w = 2; i < len; i += 4, w++) {
        dst[i+1] = reg_rd(fc->ram_base + FDCAN_RXFIFO0(get_index, w));
      }

      pb->pb_buflen = len + 4;
      pb->pb_pktlen = len + 4;
      STAILQ_INSERT_TAIL(&fc->cni.cni_ni.ni_rx_queue, pb, pb_link);
      netif_wakeup(&fc->cni.cni_ni);
    }
    reg_wr(fc->reg_base + FDCAN_RXF0A, get_index);
    return;
  }
  if(bits & (1 << 9)) {
    return;
  }
  return;
#if 0
  if(bits == 0)
    return;
  panic("bits=0x%x", bits);
#endif
}


static void
stm32_fdcan_print_info(struct device *dev, struct stream *st)
{
  stprintf(st, "hi\n");
}


static pbuf_t *
stm32_fdcan_output(can_netif_t *cni, pbuf_t *pb, uint32_t id)
{
  fdcan_t *fc = (fdcan_t *)cni;
  uint32_t txfqs = reg_rd(fc->reg_base + FDCAN_TXFQS);

  if(txfqs & (1 << 21)) {
    return pb; // Fifo full
  }

  if(pbuf_pullup(pb, pb->pb_pktlen))
    return pb;

  const void *data = pbuf_cdata(pb, 0);
  uint32_t len = pb->pb_pktlen;

  if(len > 8)
    return pb;

  int bufidx = (txfqs >> 16) & 3;
  reg_wr(fc->ram_base + FDCAN_TXBUF(bufidx, 0), signal | (1 << 30));
  reg_wr(fc->ram_base + FDCAN_TXBUF(bufidx, 1), len << 16);

  const uint32_t *u32 = data;
  for(size_t i = 0, w = 0; i < len; i += 4, w++) {
    reg_wr(fc->ram_base + FDCAN_TXBUF(bufidx, 2 + w), u32[w]);
  }

  reg_wr(fc->reg_base + FDCAN_TXBAR, 1 << bufidx);
  return pb;
}


static const device_class_t stm32_fdcan_device_class = {
  .dc_print_info = stm32_fdcan_print_info,
};


void
stm32g4_fdcan_init(int instance, gpio_t can_tx, gpio_t can_rx,
                   const struct dsig_output_filter *output_filter)
{
  fdcan_t *fc = calloc(1, sizeof(fdcan_t));
  fc->reg_base = FDCAN_BASE(instance);
  fc->ram_base = FDCAN_RAM(instance);

  reg_set_bits(RCC_CCIPR, 24, 2, 2); // FDCAN clocked from PCLK

  clk_enable(CLK_FDCAN);

  gpio_conf_af(can_tx, 9, GPIO_PUSH_PULL, GPIO_SPEED_LOW, GPIO_PULL_NONE);
  gpio_conf_af(can_rx, 9, GPIO_PUSH_PULL, GPIO_SPEED_LOW, GPIO_PULL_NONE);

  irq_enable_fn_arg(21, IRQ_LEVEL_NET, stm32g4_fdcan_irq, fc);

  for(size_t i = 0; i < 212; i++) {
    reg_wr(fc->ram_base + i * 4, 0);
  }

  reg_wr(fc->reg_base + FDCAN_CCCR, 3);

  reg_wr(fc->reg_base + FDCAN_IKE, 1);
  reg_wr(fc->reg_base + FDCAN_IE,
         (1 << 0));

  reg_wr(fc->reg_base + FDCAN_CKDIV, 0b0101);
  reg_set_bit(fc->reg_base + FDCAN_CCCR, 6);

  reg_clr_bit(fc->reg_base + FDCAN_CCCR, 0);
  while(1) {
    if((reg_rd(fc->reg_base + FDCAN_CCCR) & 1) == 0)
      break;
  }

  fc->cni.cni_output = stm32_fdcan_output;
  can_netif_attach(&fc->cni, "can", &stm32_fdcan_device_class,
                   output_filter);
}
