
#include "irq.h"

#include <mios/eventlog.h>

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include "net/can/can.h"

#include <mios/bytestream.h>

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


static const uint8_t dlc_to_len[16] = {
  0,1,2,3,4,5,6,7, 8,12,16,20,24,32,48,64
};

static void
stm32_fdcan_irq(void *arg)
{
  fdcan_t *fc = arg;

  uint32_t bits = reg_rd(fc->reg_base + FDCAN_IR);
  reg_wr(fc->reg_base + FDCAN_IR, bits);

  if(bits & (1 << 0)) {
    // RX-Fifo 0 not empty

    uint32_t rstatus = reg_rd(fc->reg_base + FDCAN_RXF0S);
    uint32_t get_index = (rstatus >> 8) & 3;

    uint32_t w1 = reg_rd(fc->ram_base + FDCAN_RXFIFO0(get_index, 1));
    uint32_t len = dlc_to_len[(w1 >> 16) & 0xf];

    pbuf_t *pb = pbuf_make(0, 0);
    if(pb != NULL) {
      uint32_t w0 = reg_rd(fc->ram_base + FDCAN_RXFIFO0(get_index, 0));
      uint32_t *dst = pbuf_data(pb, 0);
      dst[0] = w0 & 0x1fffffff;
      int words = (len + 3) / 4;
      for(int i = 0; i < words; i++) {
        dst[1 + i] = reg_rd(fc->ram_base + FDCAN_RXFIFO0(get_index, 2 + i));
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
  stprintf(st, "no info yet\n");
}


static pbuf_t *
stm32_fdcan_output(can_netif_t *cni, pbuf_t *pb, uint32_t id)
{
  fdcan_t *fc = (fdcan_t *)cni;
  uint32_t txfqs = reg_rd(fc->reg_base + FDCAN_TXFQS);

  if(pbuf_pullup(pb, pb->pb_pktlen))
    return pb;

  const void *data = pbuf_cdata(pb, 0);
  uint32_t len = pb->pb_pktlen;

  if(len > 64)
    return pb;

  uint32_t w1 = 0;

  if(len > 8) {
    w1 |= 1 << 21; // Enable FDCAN

    switch(len) {
    case 12:
      len = 9;
      break;
    case 16:
      len = 10;
      break;
    case 20:
      len = 11;
      break;
    case 24:
      len = 12;
      break;
    case 32:
      len = 13;
      break;
    case 48:
      len = 14;
      break;
    case 64:
      len = 15;
      break;
    default:
      return pb;
    }
  }
  w1 |= len << 16;

  if(txfqs & (1 << 21)) {
    return pb; // Fifo full
  }

  int bufidx = (txfqs >> 16) & 0x1f;
  reg_wr(fc->ram_base + FDCAN_TXBUF(bufidx, 0), id | (1 << 30));

  //  w1 |= 1 << 20; // Bitrate switching
  reg_wr(fc->ram_base + FDCAN_TXBUF(bufidx, 1), w1);

  const uint32_t *u32 = data;
  for(size_t i = 0, w = 0; i < pb->pb_pktlen; i += 4, w++) {
    reg_wr(fc->ram_base + FDCAN_TXBUF(bufidx, 2 + w), u32[w]);
  }
  reg_wr(fc->reg_base + FDCAN_TXBAR, 1 << bufidx);
  return pb;
}


static const device_class_t stm32_fdcan_device_class = {
  .dc_print_info = stm32_fdcan_print_info,
};


typedef struct can_timing {
  int prescaler;
  int t1;
  int t2;
} can_timing_t;

static error_t
fdcan_calculate_timings(int source_clk, int target_rate, int spa,
                        int prescaler_max, int bs1_max, int bs2_max,
                        can_timing_t *out, const char *which)
{
  int best_spa = INT32_MAX;
  error_t err = ERR_BAD_CONFIG;

  for(int p = 1; p <= prescaler_max; p++) {
    if(source_clk % (target_rate * p))
      continue;
    const int total = source_clk / (target_rate * p);
    for(int bs2 = 1; bs2 <= bs2_max; bs2++) {
      const int bs1 = total - bs2 - 1;
      if(bs1 < 1 || bs1 > bs1_max)
        continue;

      const int actual_spa = 100 * (1 + bs1) / total;
      const int diff = abs(actual_spa - spa);

      if(diff > best_spa) {
        continue;
      }

      best_spa = diff;

      out->prescaler = p;
      out->t1 = bs1;
      out->t2 = bs2;
      err = 0;
    }
  }
  return err;
}


static void
stm32_fdcan_cce(fdcan_t *fc)
{
  reg_wr(fc->reg_base + FDCAN_CCCR,
         (1 << 0) | // INIT
         (1 << 1) | // Configuration Change Enable
         0);

  while(!(reg_rd(fc->reg_base + FDCAN_CCCR) & 1)) {}
}


static error_t
stm32_fdcan_init(fdcan_t *fc, const char *name,
                 uint32_t nominal_bitrate, uint32_t data_bitrate,
                 uint32_t clockfreq,
                 const struct dsig_filter *output_filter)
{
  reg_wr(fc->reg_base + FDCAN_IKE, 1);
  reg_wr(fc->reg_base + FDCAN_IE, (1 << 0));

  can_timing_t nom, data;
  error_t err;
  err = fdcan_calculate_timings(clockfreq, nominal_bitrate, 75,
                                512, 256, 128, &nom, "nominal");
  if(err)
    return err;

  err = fdcan_calculate_timings(clockfreq, data_bitrate, 75,
                                32, 32, 16, &data, "data");
  if(err)
    return err;

  reg_wr(fc->reg_base + FDCAN_DBTP,
         ((data.prescaler - 1) << 16) |
         ((data.t1 - 1) << 8) |
         ((data.t2 - 1) << 4) |
         ((data.t2 - 1) << 0) |
         0);

  reg_wr(fc->reg_base + FDCAN_NBTP,
         ((nom.prescaler - 1) << 16) |
         ((nom.t1 - 1) << 8) |
         ((nom.t2 - 1) << 0) |
         ((nom.t2 - 1) << 25) |
         0);

  // Enable FDCAN
  reg_set_bit(fc->reg_base + FDCAN_CCCR, 8);

  // Enable bitrate switching
  reg_set_bit(fc->reg_base + FDCAN_CCCR, 9);

  // Disable automatic re-transmission
  //  reg_set_bit(fc->reg_base + FDCAN_CCCR, 6);

  // Clear init and wait for propgation
  reg_clr_bit(fc->reg_base + FDCAN_CCCR, 0);
  while(reg_rd(fc->reg_base + FDCAN_CCCR) & 1) {}

  fc->cni.cni_output = stm32_fdcan_output;
  can_netif_attach(&fc->cni, name, &stm32_fdcan_device_class,
                   output_filter);
  return 0;
}
