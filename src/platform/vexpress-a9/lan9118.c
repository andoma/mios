#include <stdio.h>
#include <stdlib.h>
#include <mios/mios.h>
#include <assert.h>

#include <sys/param.h>

#include <net/pbuf.h>
#include <net/ether.h>

#include "reg.h"
#include "irq.h"

/*
 * Note: This driver has only been tested against the emulated
 * LAN9118 chip in qemu. Most likely it won't work correctly
 * with real hardware
 */

#define LAN9118_RX_DATA_FIFO    0x00
#define LAN9118_TX_DATA_FIFO    0x20
#define LAN9118_RX_STATUS_FIFO  0x40
#define LAN9118_IRQ_CFG         0x54
#define LAN9118_INT_STS         0x58
#define LAN9118_INT_EN          0x5c
#define LAN9118_FIFO_INT        0x68
#define LAN9118_RX_CFG          0x6c
#define LAN9118_TX_CFG          0x70
#define LAN9118_RX_DP_CTRL      0x78
#define LAN9118_RX_FIFO_INF     0x7c
#define LAN9118_TX_FIFO_INF     0x80
#define LAN9118_MAC_CSR_CMD     0xa4
#define LAN9118_MAC_CSR_DATA    0xa8

#define LAN9118_MAC_CR 1

typedef struct lan9118_eth {
  ether_netif_t le_eni;

  uint32_t le_base_addr;
  uint32_t le_irq_count;
  uint32_t le_rx_error;
} lan9118_eth_t;


static void
lan9118_eth_print_info(struct device *dev, struct stream *st)
{
  lan9118_eth_t *le = (lan9118_eth_t *)dev;
  ether_print(&le->le_eni, st);
  stprintf(st, "\tIRQ count:%d\n", le->le_irq_count);
  stprintf(st, "\tRXFifoInf: 0x%08x\n",
           reg_rd(le->le_base_addr + LAN9118_RX_FIFO_INF));
  stprintf(st, "\tMac error status counter:%d\n",
           le->le_rx_error);

}

static const device_class_t lan9118_eth_device_class = {
  .dc_print_info = lan9118_eth_print_info,
};

static error_t
lan9118_eth_output(struct ether_netif *eni, pbuf_t *pkt, int flags)
{
  lan9118_eth_t *le = (lan9118_eth_t *)eni;
  pbuf_t *pb;

  le->le_eni.eni_stats.tx_pkt++;
  le->le_eni.eni_stats.tx_byte += pkt->pb_pktlen;

  for(pb = pkt; pb != NULL; pb = pb->pb_next) {

    uint32_t copy_offset = pb->pb_offset & ~3;
    uint32_t start_offset = pb->pb_offset & 3;

    uint32_t tx_a = start_offset << 16;
    if(pb->pb_flags & PBUF_SOP)
      tx_a |= 1 << 13;
    if(pb->pb_flags & PBUF_EOP)
      tx_a |= 1 << 12;

    tx_a |= pb->pb_buflen;

    reg_wr(le->le_base_addr + LAN9118_TX_DATA_FIFO, tx_a);

    reg_wr(le->le_base_addr + LAN9118_TX_DATA_FIFO, pkt->pb_pktlen);

    uint32_t words = (start_offset + pb->pb_buflen + 3) >> 2;
    const uint32_t *src = pb->pb_data + copy_offset;

    for(size_t i = 0; i < words; i++) {
      reg_wr(le->le_base_addr + LAN9118_TX_DATA_FIFO, src[i]);
    }
  }
  pbuf_free(pkt);
  return 0;
}


static void
lan9118_irq(void *arg)
{
  lan9118_eth_t *le = arg;
  le->le_irq_count++;
  uint32_t irq_status = reg_rd(le->le_base_addr + LAN9118_INT_STS);
  if(!(irq_status & 8))
    return;

  // RX

  int wakeup = 0;
  while(1) {

    const uint32_t rx_fifo_inf = reg_rd(le->le_base_addr + LAN9118_RX_FIFO_INF);
    int fifo_len = rx_fifo_inf & 0xffff;
    int packets = rx_fifo_inf >> 16;
    if(!packets)
      break;

    uint32_t packet_status = reg_rd(le->le_base_addr + LAN9118_RX_STATUS_FIFO);
    int packet_len = (packet_status >> 16) & 0x3fff;
    if(packet_status & 0x8000) {
      le->le_rx_error++;
    }
    int offset = 2;
    int bytes = (packet_len + offset + 3) & ~3;
    int tail = bytes - packet_len - 2;
    assert(fifo_len >= bytes);

    struct pbuf_queue pbq;
    STAILQ_INIT(&pbq);

    pbuf_t *pb = pbuf_get(0);
    if(pb == NULL) {
      le->le_eni.eni_stats.rx_sw_qdrop++;
    } else {
      pb->pb_pktlen = packet_len;
      pb->pb_data = 0;
      pb->pb_flags = PBUF_SOP;
    }

    while(bytes) {
      void *buf = NULL;

      if(pb) {
        buf = pbuf_data_get(0);
        if(buf == NULL) {
          pbuf_put(pb);
          pb = NULL;
          pbuf_free_queue_irq_blocked(&pbq);
          le->le_eni.eni_stats.rx_sw_qdrop++;
        }
      }

      const int to_copy = MIN(bytes, PBUF_DATA_SIZE);

      if(pb) {
        STAILQ_INSERT_TAIL(&pbq, pb, pb_link);
        pb->pb_data = buf;
        pb->pb_offset = offset;
        pb->pb_buflen = to_copy - offset;
        uint32_t *u32 = buf;
        for(size_t i = 0; i < to_copy >> 2; i++) {
          u32[i] = reg_rd(le->le_base_addr + LAN9118_RX_DATA_FIFO);
        }
      } else {
        for(size_t i = 0; i < to_copy >> 2; i++) {
          reg_rd(le->le_base_addr + LAN9118_RX_DATA_FIFO);
        }
      }

      offset = 0;
      bytes -= to_copy;

      if(bytes == 0) {

        if(pb) {
          pb->pb_flags |= PBUF_EOP;
          pb->pb_buflen -= tail;

          le->le_eni.eni_stats.rx_byte += packet_len;
          le->le_eni.eni_stats.rx_pkt++;
          STAILQ_CONCAT(&le->le_eni.eni_ni.ni_rx_queue, &pbq);
        }
        wakeup = 1;
        break;
      }

      if(pb != NULL) {
        pb = pbuf_get(0);
        if(pb == NULL) {
          pbuf_free_queue_irq_blocked(&pbq);
          le->le_eni.eni_stats.rx_sw_qdrop++;
        } else {
          pb->pb_flags = 0;
        }
      }
    }
  }

  if(wakeup)
    netif_wakeup(&le->le_eni.eni_ni);

  reg_wr(le->le_base_addr + LAN9118_INT_STS, irq_status);

}


static void
lan9118_mac_wr(lan9118_eth_t *le, uint8_t reg, uint32_t value)
{
  reg_wr(le->le_base_addr + LAN9118_MAC_CSR_DATA, value);
  reg_wr(le->le_base_addr + LAN9118_MAC_CSR_CMD, (1 << 31) | reg);
}



static void
lan9118_init(uint32_t base_addr, int irq)
{
  lan9118_eth_t *le = calloc(1, sizeof(lan9118_eth_t));

  // FIX THIS
  le->le_eni.eni_addr[0] = 0x06;
  le->le_eni.eni_addr[1] = 0x00;
  le->le_eni.eni_addr[5] = 0x42;

  le->le_base_addr = base_addr;

  //  uint32_t rev = reg_rd(base_addr + 0x50);
  //  uint32_t order = reg_rd(base_addr + 0x64);

  le->le_eni.eni_output = lan9118_eth_output;

  ether_netif_init(&le->le_eni, "eth0", &lan9118_eth_device_class);

  net_task_raise(&le->le_eni.eni_ni.ni_task, NETIF_TASK_STATUS_UP);

  reg_wr(base_addr + LAN9118_TX_CFG, 6);
  reg_wr(base_addr + LAN9118_RX_CFG, (2 << 8));
  reg_wr(base_addr + LAN9118_INT_EN, 0x8);
  reg_wr(base_addr + LAN9118_IRQ_CFG,
         (1 << 8) |
         (1 << 4) |
         (1 << 0));

  lan9118_mac_wr(le, LAN9118_MAC_CR,
                 (1 << 19) |
                 (1 << 18) |
                 (1 << 3) |
                 (1 << 2));

  irq_enable_fn_arg(irq, IRQ_LEVEL_NET, lan9118_irq, le);
}


static void  __attribute__((constructor(400)))
vexpress_a9_init_ethernet(void)
{
  lan9118_init(0x4e000000, 32 + 15);
}
