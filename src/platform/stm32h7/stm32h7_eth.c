#include <assert.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <mios/io.h>
#include <mios/eventlog.h>
#include <malloc.h>
#include <unistd.h>

#include <net/pbuf.h>
#include <net/ether.h>

#include <util/crc32.h>

#include "irq.h"
#include "stm32h7_clk.h"
#include "stm32h7_eth.h"

#define DMA_BUFFER_PAD 2


typedef struct {
  union {
    uint32_t w0;
    void *p0;
  };
  uint32_t w1;
  uint32_t w2;
  uint32_t w3;
} desc_t;

#define ETH_RDES3_OWN           0x80000000
#define ETH_RDES3_IOC           0x40000000
#define ETH_RDES3_FD            0x20000000
#define ETH_RDES3_LD            0x10000000
#define ETH_RDES3_BUF2V         0x02000000
#define ETH_RDES3_BUF1V         0x01000000

#define ETH_TDES3_OWN           0x80000000
#define ETH_TDES3_FD            0x20000000
#define ETH_TDES3_LD            0x10000000

#define SYSCFG_BASE 0x58000400


#define ETH_BASE 0x40028000

#define ETH_MACCR   (ETH_BASE + 0x0)
#define ETH_MACECR  (ETH_BASE + 0x4)
#define ETH_MACPFR  (ETH_BASE + 0x8)
#define ETH_MACWTR  (ETH_BASE + 0xc)
#define ETH_MACHT0R (ETH_BASE + 0x10)
#define ETH_MACHT1R (ETH_BASE + 0x14)

#define ETH_MACVTR     (ETH_BASE + 0x50)
#define ETH_MACVHTR    (ETH_BASE + 0x58)
#define ETH_MACVIR     (ETH_BASE + 0x60)
#define ETH_MACQTXFCR  (ETH_BASE + 0x70)
#define ETH_MACRXFCR   (ETH_BASE + 0x90)
#define ETH_MACISR     (ETH_BASE + 0xb0)
#define ETH_MACIER     (ETH_BASE + 0xb4)
#define ETH_MACRXTXSR  (ETH_BASE + 0xb8)
#define ETH_MACPCSR    (ETH_BASE + 0xc0)
#define ETH_MACWKPFR   (ETH_BASE + 0xc4)
#define ETH_MACLCSR    (ETH_BASE + 0xd0)
#define ETH_MACLTCR    (ETH_BASE + 0xd4)
#define ETH_MACLETR    (ETH_BASE + 0xd8)
#define ETH_MAC1USTCR  (ETH_BASE + 0xdc)
#define ETH_MACVR      (ETH_BASE + 0x110)
#define ETH_MACDR      (ETH_BASE + 0x114)
#define ETH_MACHWF1R   (ETH_BASE + 0x120)
#define ETH_MACHWF2R   (ETH_BASE + 0x124)

#define ETH_MACMDIOAR  (ETH_BASE + 0x200)
#define ETH_MACMDIODR  (ETH_BASE + 0x204)
#define ETH_MACARPAR   (ETH_BASE + 0x210)

#define ETH_MACAHR(x)  (ETH_BASE + 0x300 + 8 * (x))
#define ETH_MACALR(x)  (ETH_BASE + 0x304 + 8 * (x))

#define ETH_MMC_CONTROL       (ETH_BASE + 0x700)
#define ETH_MMC_RX_INTERRUPT  (ETH_BASE + 0x704)
#define ETH_MMC_TX_INTERRUPT  (ETH_BASE + 0x708)
#define ETH_MMC_RX_INTR_MASK  (ETH_BASE + 0x70c)
#define ETH_MMC_TX_INTR_MASK  (ETH_BASE + 0x710)

#define ETH_MTLOMR       (ETH_BASE + 0xc00)
#define ETH_MTLISR       (ETH_BASE + 0xc20)
#define ETH_MTLTXQOMR    (ETH_BASE + 0xd00)
#define ETH_MTLTXQUR     (ETH_BASE + 0xd04)
#define ETH_MTLTXQDR     (ETH_BASE + 0xd08)
#define ETH_MTLQICSR     (ETH_BASE + 0xd2c)
#define ETH_MTLRXQOMR    (ETH_BASE + 0xd30)
#define ETH_MTLRXQMPOCR  (ETH_BASE + 0xd34)
#define ETH_MTLRXQDR     (ETH_BASE + 0xd38)


#define ETH_DMAMR        (ETH_BASE + 0x1000)
#define ETH_DMASBMR      (ETH_BASE + 0x1004)

#define ETH_DMACCR       (ETH_BASE + 0x1100)
#define ETH_DMACTXCR     (ETH_BASE + 0x1104)
#define ETH_DMACRXCR     (ETH_BASE + 0x1108)

#define ETH_DMACTXDLAR   (ETH_BASE + 0x1114)
#define ETH_DMACRXDLAR   (ETH_BASE + 0x111c)

#define ETH_DMACTXDTPR   (ETH_BASE + 0x1120)
#define ETH_DMACRXDTPR   (ETH_BASE + 0x1128)


#define ETH_DMACTXRLR    (ETH_BASE + 0x112c)
#define ETH_DMACRXRLR    (ETH_BASE + 0x1130)

#define ETH_DMACIER      (ETH_BASE + 0x1134)

#define ETH_DMACSR       (ETH_BASE + 0x1160)



#define ETH_TX_RING_SIZE 16
#define ETH_TX_RING_MASK (ETH_TX_RING_SIZE - 1)

#define ETH_RX_RING_SIZE 8
#define ETH_RX_RING_MASK (ETH_RX_RING_SIZE - 1)

typedef struct stm32h7_eth {
  ether_netif_t se_eni;

  struct pbuf_queue se_rx_scatter_queue;
  int se_rx_scatter_length;

  desc_t *se_txring;
  desc_t *se_rxring;

  void *se_tx_pbuf_data[ETH_TX_RING_SIZE];

  uint8_t se_next_rx; // rename


  uint8_t se_tx_rdptr; // Where DMA currently is
  uint8_t se_tx_wrptr; // Where we will write next TX desc

  uint8_t se_phyaddr;

} stm32h7_eth_t;


static stm32h7_eth_t stm32h7_eth;



static void
rx_desc_give(desc_t *rx, void *buf)
{
  rx->p0 = buf + DMA_BUFFER_PAD;
  rx->w3 = ETH_RDES3_OWN | ETH_RDES3_IOC | ETH_RDES3_BUF1V;
  asm volatile ("dsb");
  reg_wr(ETH_DMACRXDTPR, (uint32_t)rx);
}


static void
stm32h7_eth_print_info(struct device *dev, struct stream *st)
{
  ether_print((ether_netif_t *)dev, st);
}


static const device_class_t stm32h7_eth_device_class = {
  .dc_print_info = stm32h7_eth_print_info,
};

static uint16_t
mii_read(void *arg, uint16_t reg)
{
  stm32h7_eth_t *se = arg;
  int phyaddr = se->se_phyaddr;

  reg_wr(ETH_MACMDIOAR,
         (phyaddr << 21) |
         (reg << 16) |
         (0 << 12) |
         (0 << 8) |
         (3 << 2) |
         (1 << 0));

  while(reg_rd(ETH_MACMDIOAR) & 1) {
    if(can_sleep()) {
      usleep(10);
    }
  }
  return reg_rd(ETH_MACMDIODR) & 0xffff;
}



static void
mii_write(void *arg, uint16_t reg, uint16_t value)
{
  stm32h7_eth_t *se = arg;
  int phyaddr = se->se_phyaddr;

  reg_wr(ETH_MACMDIODR, value);
  reg_wr(ETH_MACMDIOAR,
         (phyaddr << 21) |
         (reg << 16) |
         (1 << 2) |
         (1 << 0));
  while(reg_rd(ETH_MACMDIOAR) & 1) {
    if(can_sleep()) {
      usleep(10);
    }
  }
}


static void *__attribute__((noreturn))
stm32h7_phy_thread(void *arg)
{
  stm32h7_eth_t *se = arg;
  int current_up = 0;
  while(1) {
    mii_read(se, 1);
    int n = mii_read(se, 1);
    int up = !!(n & 4);

    if(!current_up && up) {
      current_up = 1;
      net_task_raise(&se->se_eni.eni_ni.ni_task, NETIF_TASK_STATUS_UP);
    } else if(current_up && !up) {
      current_up = 0;
      net_task_raise(&se->se_eni.eni_ni.ni_task, NETIF_TASK_STATUS_DOWN);
    }
    usleep(100000);
  }
}


static void
handle_irq_rx(stm32h7_eth_t *se)
{
  while(1) {
    desc_t *rx = se->se_rxring + (se->se_next_rx & ETH_RX_RING_MASK);
    const uint32_t w3 = rx->w3;
    if(w3 & ETH_RDES3_OWN)
      break;
    const int len = w3 & 0x7fff;

    if(w3 & ETH_RDES3_FD) {
      pbuf_t *pb = STAILQ_FIRST(&se->se_rx_scatter_queue);
      if(pb != NULL)
        pbuf_free_irq_blocked(pb);
      se->se_rx_scatter_length = 0;
    }

    void *buf = rx->p0;
    pbuf_t *pb = pbuf_get(0);
    if(pb != NULL) {
      int flags = 0;

      if(w3 & ETH_RDES3_FD) {
        flags |= PBUF_SOP;
        pb->pb_offset = 2;
      } else {
        pb->pb_offset = 0;
      }
      if(w3 & ETH_RDES3_LD)
        flags |= PBUF_EOP;

      pb->pb_data = buf - DMA_BUFFER_PAD;
      pb->pb_flags = flags;

      if(flags == (PBUF_SOP | PBUF_EOP)) {
        pb->pb_buflen = len;
        pb->pb_pktlen = len;
        STAILQ_INSERT_TAIL(&se->se_eni.eni_ni.ni_rx_queue, pb, pb_link);
        netif_wakeup(&se->se_eni.eni_ni);
      } else {

        STAILQ_INSERT_TAIL(&se->se_rx_scatter_queue, pb, pb_link);

        pb->pb_buflen = len - se->se_rx_scatter_length;

        if(flags & PBUF_EOP) {
          pbuf_t *first = STAILQ_FIRST(&se->se_rx_scatter_queue);
          first->pb_pktlen = len;

          STAILQ_CONCAT(&se->se_eni.eni_ni.ni_rx_queue,
                        &se->se_rx_scatter_queue);
          netif_wakeup(&se->se_eni.eni_ni);

        } else {
          se->se_rx_scatter_length = len;
        }
      }

      // Get new buffer for DMA RX
      buf = pbuf_data_get(0);
      if(buf == NULL) {
        panic("Out of buffers");
        break;
      }
    }

    rx_desc_give(rx, buf);
    se->se_next_rx++;
  }
}


static void
handle_irq_tx(stm32h7_eth_t *se)
{
  while(se->se_tx_rdptr != se->se_tx_wrptr) {
    desc_t *tx = se->se_txring + (se->se_tx_rdptr & ETH_TX_RING_MASK);

    const uint32_t w3 = tx->w3;
    if(w3 & ETH_TDES3_OWN)
      break;
    pbuf_data_put(se->se_tx_pbuf_data[se->se_tx_rdptr & ETH_TX_RING_MASK]);
    se->se_tx_rdptr++;
  }
}



static error_t
stm32h7_eth_output(struct ether_netif *eni, pbuf_t *pkt, int flags)
{
  stm32h7_eth_t *se = (stm32h7_eth_t *)eni;

  pbuf_t *pb;
  int q = irq_forbid(IRQ_LEVEL_NET);

  int wrptr = se->se_tx_wrptr;
  for(pb = pkt; pb != NULL; pb = pb->pb_next) {

    desc_t *tx = se->se_txring + (wrptr & ETH_TX_RING_MASK);
    se->se_tx_pbuf_data[wrptr & ETH_TX_RING_MASK] = pb->pb_data;

    tx->p0 = pb->pb_data + pb->pb_offset;
    tx->w1 = 0;
    tx->w2 = (1 << 31) | pb->pb_buflen;

    uint32_t w3 = ETH_TDES3_OWN | pb->pb_pktlen;

    w3 |= (0b11 << 16); // Calculate and insert checksums

    if(pb->pb_flags & PBUF_SOP)
      w3 |= ETH_TDES3_FD;
    if(pb->pb_flags & PBUF_EOP) {
      w3 |= ETH_TDES3_LD;
    }
    tx->w3 = w3;

    wrptr++;
  }

  desc_t *tx = se->se_txring + (wrptr & ETH_TX_RING_MASK);
  asm volatile ("dsb");
  reg_wr(ETH_DMACTXDTPR, (uint32_t)tx);

  se->se_tx_wrptr = wrptr;
  for(; pkt != NULL; pkt = pb) {
    pb = STAILQ_NEXT(pkt, pb_link);
    pbuf_put(pkt); // Free header, data will be free'd after TX is done
  }

  irq_permit(q);
  return 0;
}





static void
stm32h7_eth_irq(void *arg)
{
  stm32h7_eth_t *se = arg;

  const uint32_t dmacsr = reg_rd(ETH_DMACSR);
  const uint32_t mtlisr = reg_rd(ETH_MTLISR);
  const uint32_t macisr = reg_rd(ETH_MACISR);

  if(dmacsr & ~0x8c45) {
    panic("%s: Unhandled dmairq 0x%x", __FUNCTION__, dmacsr & ~0x8c45);
  }
  assert(mtlisr == 0);
  assert(macisr == 0);

  if(dmacsr & 0x5)
    handle_irq_tx(se);

  if(dmacsr & 0x40)
    handle_irq_rx(se);

  reg_wr(ETH_DMACSR, dmacsr);
}

static const ethphy_reg_io_t stm32h7_eth_mdio = {
  .read = mii_read,
  .write = mii_write
};


void
stm32h7_eth_init(gpio_t phyrst, const uint8_t *gpios, size_t gpio_count,
                 const ethphy_driver_t *ethphy, int phy_addr,
                 ethphy_mode_t mode)
{
  stm32h7_eth_t *se = &stm32h7_eth;

  clk_enable(CLK_SYSCFG);

  if(mode == ETHPHY_MODE_RMII) {
    reg_set_bits(SYSCFG_BASE + 0x4, 21, 3, 4);
  } else {
    reg_set_bits(SYSCFG_BASE + 0x4, 21, 3, 0);
  }

  for(size_t i = 0; i < gpio_count; i++) {
    gpio_conf_af(gpios[i], 11, GPIO_PUSH_PULL,
                 GPIO_SPEED_VERY_HIGH, GPIO_PULL_NONE);
  }

  if(phyrst != GPIO_UNUSED) {
    gpio_conf_output(phyrst, GPIO_PUSH_PULL,
                     GPIO_SPEED_LOW, GPIO_PULL_NONE);
    gpio_set_output(phyrst, 0);
    udelay(10);
    gpio_set_output(phyrst, 1);
    udelay(10);
  }

  clk_enable(CLK_ETH1MACEN);
  clk_enable(CLK_ETH1TXEN);
  clk_enable(CLK_ETH1RXEN);

  reset_peripheral(CLK_ETH1MACEN);
  se->se_phyaddr = phy_addr;

  if(ethphy) {
    error_t err = ethphy->init(mode, &stm32h7_eth_mdio, se);
    if(err) {
      evlog(LOG_ERR, "stm32h7: PHY init failed");
      return;
    }
  }

  se->se_eni.eni_addr[0] = 0x02;
  se->se_eni.eni_addr[1] = 0x00;

  uint32_t uuidcrc = crc32(0, (void *)0x1ff1e800, 12);
  memcpy(&se->se_eni.eni_addr[2], &uuidcrc, 4);

  se->se_tx_wrptr = 0;
  se->se_next_rx = 0;
  STAILQ_INIT(&se->se_rx_scatter_queue);
  se->se_txring = xalloc(sizeof(desc_t) * ETH_TX_RING_SIZE, 0,
                         MEM_TYPE_DMA | MEM_TYPE_NO_CACHE);
  se->se_rxring = xalloc(sizeof(desc_t) * ETH_RX_RING_SIZE, 0,
                         MEM_TYPE_DMA | MEM_TYPE_NO_CACHE);

  // Soft reset
  reg_set_bit(ETH_DMAMR, 0);

  int i = 0;
  while(reg_rd(ETH_DMAMR) & 1) {
    i++;
    if(i == 1000000) {
      evlog(LOG_ERR, "Ethernet MAC failed to initialize, no clock?");
      return;
    }
  }
  udelay(10);

  reg_wr(ETH_MACPFR, 1 << 31);  // Receive ALL

  reg_set_bit(ETH_MTLTXQOMR, 1);

  reg_set_bits(ETH_DMACTXCR, 16, 5, 1);

  reg_set_bits(ETH_DMACRXCR, 16, 5, 1);

  reg_set_bits(ETH_DMACRXCR, 1, 14, PBUF_DATA_SIZE - DMA_BUFFER_PAD);

  memset(se->se_txring, 0, sizeof(desc_t) * ETH_TX_RING_SIZE);
  memset(se->se_rxring, 0, sizeof(desc_t) * ETH_RX_RING_SIZE);

  reg_wr(ETH_DMACTXDLAR, (uint32_t)se->se_txring);
  reg_wr(ETH_DMACTXRLR,  ETH_TX_RING_SIZE - 1);
  reg_wr(ETH_DMACTXDTPR, (uint32_t)se->se_txring);

  reg_wr(ETH_DMACRXDLAR, (uint32_t)se->se_rxring);
  reg_wr(ETH_DMACRXRLR,  ETH_RX_RING_SIZE - 1);

  for(int i = 0; i < ETH_RX_RING_SIZE; i++) {
    void *buf = pbuf_data_get(0);
    if(buf == NULL)
      break;
    desc_t *rx = se->se_rxring + i;
    rx_desc_give(rx, buf);
  }

  reg_wr(ETH_DMACIER, (1 << 15) | (1 << 6) | (1 << 2) | (1 << 0));

  reg_set_bit(ETH_MACCR, 21); // CST: Strip CRC for type packets in RX path
  //  reg_set_bit(ETH_MACCR, 20); // ACS: Strip CRC in RX path

  reg_set_bit(ETH_MACCR, 0); // Enable RX
  reg_set_bit(ETH_MACCR, 1); // Enable TX

  reg_set_bit(ETH_MACCR, 14); // 100Mbit
  reg_set_bit(ETH_MACCR, 13); // FullDuplex

  // Start DMA
  reg_set_bit(ETH_DMACRXCR, 0);
  reg_set_bit(ETH_DMACTXCR, 0);

  se->se_eni.eni_output = stm32h7_eth_output;

  se->se_eni.eni_ni.ni_flags |=
    NETIF_F_TX_IPV4_CKSUM_OFFLOAD |
    NETIF_F_TX_ICMP_CKSUM_OFFLOAD |
    NETIF_F_TX_UDP_CKSUM_OFFLOAD |
    NETIF_F_TX_TCP_CKSUM_OFFLOAD;

  ether_netif_init(&se->se_eni, "eth0", &stm32h7_eth_device_class);

  irq_enable_fn_arg(61, IRQ_LEVEL_NET, stm32h7_eth_irq, se);
  thread_create(stm32h7_phy_thread, se, 512, "phy", 0, 4);
}
