#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <sys/param.h>

#include <net/can/can.h>
#include <net/net.h>

#include <mios/bytestream.h>
#include <mios/dsig.h>

#include "irq.h"

#define CAN_MCR      0x000
#define CAN_MSR      0x004
#define CAN_TSR      0x008
#define CAN_RF(x)   (0x00c + (x) * 4)
#define CAN_IER      0x014
#define CAN_BTR      0x01c

#define CAN_TI(x)   (0x180 + (x) * 0x10)
#define CAN_TDT(x)  (0x184 + (x) * 0x10)
#define CAN_TDL(x)  (0x188 + (x) * 0x10)
#define CAN_TDH(x)  (0x18c + (x) * 0x10)

#define CAN_RI(x)   (0x1b0 + (x) * 0x10)
#define CAN_RDT(x)  (0x1b4 + (x) * 0x10)
#define CAN_RDL(x)  (0x1b8 + (x) * 0x10)
#define CAN_RDH(x)  (0x1bc + (x) * 0x10)

#define CAN_FMR      0x200
#define CAN_FM1R     0x204
#define CAN_FS1R     0x20c
#define CAN_FFA1R    0x214
#define CAN_FA1R     0x21c

#define CAN_FxR1(x) (0x240 + (x) * 8)
#define CAN_FxR2(x) (0x244 + (x) * 8)

#define TX_MAILBOXES 3

typedef struct bxcan {
  can_netif_t cni;

  struct pbuf_queue tx_queue[TX_MAILBOXES];

  uint8_t tx_status[TX_MAILBOXES];

  uint32_t reg_base;

  uint32_t queued;

  char name[5];

} bxcan_t;

static void
stm32_bxcan_send(bxcan_t *bx, pbuf_t *pb, uint32_t id, int mailbox)
{
  uint32_t reg_base = bx->reg_base;

  int len = MIN(pb->pb_buflen, 8);

  const void *data = pbuf_data(pb, 0);

  if(len) {
    uint32_t u32 = 0;
    memcpy(&u32, data, MIN(4, len));
    reg_wr(reg_base + CAN_TDL(mailbox), u32);
    if(len > 3) {
      u32 = 0;
      memcpy(&u32, data + 4, MIN(4, len - 4));
      reg_wr(reg_base + CAN_TDH(mailbox), u32);
    }
  }

  reg_wr(reg_base + CAN_TDT(mailbox), len);
  reg_wr(reg_base + CAN_TI(mailbox), (id << 3) | 0x5);
  bx->tx_status[mailbox] = 1;
}



static pbuf_t *
stm32_bxcan_output(can_netif_t *cni, pbuf_t *pb, uint32_t id)
{
  bxcan_t *bx = (bxcan_t *)cni;
  int mailbox = 0;

  int q = irq_forbid(IRQ_LEVEL_NET);
  if(bx->tx_status[mailbox]) {
    pb = pbuf_prepend(pb, 4, 0, 0);
    if(pb != NULL) {
      wr32_le(pbuf_data(pb, 0), id);
      if(!pbuf_pullup(pb, pb->pb_pktlen)) {
        STAILQ_INSERT_TAIL(&bx->tx_queue[mailbox], pb, pb_link);
        bx->queued++;
        pb = NULL;
      }
    }

  } else {

    if(!pbuf_pullup(pb, pb->pb_pktlen)) {
      stm32_bxcan_send(bx, pb, id, mailbox);
    }
  }
  irq_permit(q);

  return pb;
}


static void
stm32_bxcan_tx_irq(void *arg)
{
  bxcan_t *bx = arg;

  uint32_t tsr = reg_rd(bx->reg_base + CAN_TSR);
  for(int i = 0; i < TX_MAILBOXES; i++) {
    uint32_t mbstatus = (tsr >> (i * 8)) & 0xff;
    if(!(mbstatus & 1))
      continue;
    reg_wr(bx->reg_base + CAN_TSR, 1 << (i * 8));
    pbuf_t *pb = pbuf_splice(&bx->tx_queue[i]);
    if(pb == NULL) {
      bx->tx_status[i] = 0;
    } else {
      uint32_t group = rd32_le(pbuf_data(pb, 0));
      pb = pbuf_drop(pb, 4);
      stm32_bxcan_send(bx, pb, group, i);
    }
  }
}


static void
stm32_bxcan_rx(bxcan_t *bx, int mailbox)
{
  while(1) {
    uint32_t rf = reg_rd(bx->reg_base + CAN_RF(mailbox));
    uint32_t fmp = rf & 0x3;
    if(!fmp)
      break;

    uint32_t ri = reg_rd(bx->reg_base + CAN_RI(mailbox));
    uint32_t rdt = reg_rd(bx->reg_base + CAN_RDT(mailbox));

    pbuf_t *pb = pbuf_make_irq_blocked(0, 0);
    if(pb != NULL) {
      const uint32_t id = ri >> (ri & 4 ? 3 : 21);
      uint32_t len = rdt & 0xf;

      uint8_t *data = pbuf_data(pb, 0);
      wr32_le(data, id);
      pb->pb_pktlen = pb->pb_buflen = 4 + len;
      if(len) {
        uint32_t u32;

        u32 = reg_rd(bx->reg_base + CAN_RDL(mailbox));
        memcpy(data + 4, &u32, MIN(len, 4));
        if(len > 4) {
          u32 = reg_rd(bx->reg_base + CAN_RDH(mailbox));
          memcpy(data + 8, &u32, MIN(len - 4, 4));
        }
      }
      STAILQ_INSERT_TAIL(&bx->cni.cni_ni.ni_rx_queue, pb, pb_link);
      netif_wakeup(&bx->cni.cni_ni);
    }
    reg_wr(bx->reg_base + CAN_RF(mailbox), 1 << 5);
  }
}

static void
stm32_bxcan_rx0_irq(void *arg)
{
  stm32_bxcan_rx(arg, 0);
}

static void
stm32_bxcan_rx1_irq(void *arg)
{
  stm32_bxcan_rx(arg, 1);
}

static void
stm32_bxcan_sce_irq(void *arg)
{

}


static void
stm32_bxcan_print_info(struct device *dev, struct stream *st)
{
}

static const device_class_t stm32_bxcan_device_class = {
  .dc_print_info = stm32_bxcan_print_info,
};



static void
set_filter_bank(bxcan_t *bx, int bank, uint32_t id, uint32_t mask,
                uint32_t flags)
{
  reg_clr_bit(bx->reg_base + CAN_FM1R, bank); // Mask mode
  reg_set_bit(bx->reg_base + CAN_FS1R, bank); // 32bit mode
  reg_clr_bit(bx->reg_base + CAN_FFA1R, bank); // RXFIFO 0/1

  uint32_t r1 = 0, r2 = 0;
  if(mask) {
    if(flags & DSIG_FLAG_EXTENDED) {
      r1 = (id << 3) | 0x4;
      r2 = (mask << 3) | 0x4;
    } else {
      r1 = (id << 21);
      r2 = (mask << 21) | 0x4;
    }
  }

  reg_wr(bx->reg_base + CAN_FxR1(bank), r1);
  reg_wr(bx->reg_base + CAN_FxR2(bank), r2);
  reg_set_bit(bx->reg_base + CAN_FA1R, bank); // Activate
}

static void
set_filter_from_list(bxcan_t *bx, const struct dsig_filter *dif)
{
  int bank = 0;
  while(dif->prefixlen != 0xff) {
    if(bank == 0xe) {
      panic("%s: out of filter banks", bx->name);
    }

    uint32_t prefix = dif->prefix;
    uint32_t mask = mask_from_prefixlen(dif->prefixlen);
    set_filter_bank(bx, bank, prefix, mask, dif->flags);
    dif++;
    bank++;
  }
}

static uint32_t
calc_btr(int clk_ratio, int spa)
{
  uint32_t btr = 0;
  int best = INT32_MAX;
  for(int i = 1; i < 1024; i++) {
    if(clk_ratio % i)
      continue;

    uint32_t q = clk_ratio / i;
    int ts1 = ((q * spa + 50) / 100) - 1;
    int ts2 = q - 1 - ts1;
    if(ts1 > 16 || ts2 > 8) {
      continue;
    }

    if(ts1 == 0 || ts2 == 0)
      break;

    int actual_spa = 100 * (1 + ts1) / (1 + ts1 + ts2);
    int d = spa - actual_spa;
    if(d < 0)
      d = -d;

    if(d > best)
      continue;

    best = d;
    btr =
      (i - 1) |
      (ts1 - 1) << 16 |
      (ts2 - 1) << 20;
  }
  return btr;
}


static void
stm32_bxcan_init(int instance, int bitrate, int irq_base,
                 const struct dsig_filter *input_filter,
                 const struct dsig_filter *output_filter)
{
  bxcan_t *bx = calloc(1, sizeof(bxcan_t));

  for(int i = 0; i < TX_MAILBOXES; i++) {
    STAILQ_INIT(&bx->tx_queue[i]);
  }

  uint16_t clkid = CLK_CAN(instance);
  clk_enable(clkid);

  uint32_t btr = calc_btr(clk_get_freq(clkid) / bitrate, 85);
  uint32_t reg_base = CAN_BASE(instance);
  bx->reg_base = reg_base;

  irq_enable_fn_arg(irq_base + 0, IRQ_LEVEL_NET, stm32_bxcan_tx_irq, bx);
  irq_enable_fn_arg(irq_base + 1, IRQ_LEVEL_NET, stm32_bxcan_rx0_irq, bx);
  irq_enable_fn_arg(irq_base + 2, IRQ_LEVEL_NET, stm32_bxcan_rx1_irq, bx);
  irq_enable_fn_arg(irq_base + 3, IRQ_LEVEL_NET, stm32_bxcan_sce_irq, bx);

  // Enter INIT mode
  reg_set_bit(reg_base + CAN_MCR, 0);
  while(!(reg_rd(reg_base + CAN_MSR) & 1)) {
  }

  reg_wr(reg_base + CAN_BTR, btr);
  reg_wr(reg_base + CAN_MCR, 0x10001);

  // Enter Normal mode
  reg_clr_bit(reg_base + CAN_MCR, 0);
  while((reg_rd(reg_base + CAN_MSR) & 1)) {
  }

  if(input_filter == NULL) {
    set_filter_bank(bx, 0, 0, 0, 0);
  } else {
    set_filter_from_list(bx, input_filter);
  }
  reg_clr_bit(reg_base + CAN_FMR, 0);

  bx->cni.cni_output = stm32_bxcan_output;
  snprintf(bx->name, sizeof(bx->name), "can%d", instance);
  can_netif_attach(&bx->cni, bx->name, &stm32_bxcan_device_class,
                   output_filter);

  reg_wr(reg_base + CAN_IER,
         (1 << 5) |
         (1 << 1) |
         (1 << 0));
}
