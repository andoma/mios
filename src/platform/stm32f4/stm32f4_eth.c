#include "stm32f4_eth.h"
#include "stm32f4_reg.h"
#include "stm32f4_clk.h"

#include <net/pbuf.h>
#include <net/ether.h>

#include <stdio.h>
#include <malloc.h>
#include <unistd.h>
#include <string.h>

#include <mios/mios.h>
#include <mios/eventlog.h>
#include <mios/sys.h>

#include <util/crc32.h>

#include "irq.h"

#define DMA_BUFFER_PAD 2

#define ETH_TX_RING_SIZE 16
#define ETH_RX_RING_SIZE 8

#define ETH_TX_RING_MASK (ETH_TX_RING_SIZE - 1)
#define ETH_RX_RING_MASK (ETH_RX_RING_SIZE - 1)

#define ETH_BASE   0x40028000

// MAC

#define ETH_MACCR    (ETH_BASE + 0x0000)
#define ETH_MACFFR   (ETH_BASE + 0x0004)
#define ETH_MACMIIAR (ETH_BASE + 0x0010)
#define ETH_MACMIIDR (ETH_BASE + 0x0014)
#define ETH_MACDBGR  (ETH_BASE + 0x0034)
#define ETH_MACIMR   (ETH_BASE + 0x003c)

// MMC
#define ETH_MMCCR     (ETH_BASE + 0x0100)
#define ETH_MMCRFCECR (ETH_BASE + 0x0194)
#define ETH_MMCRGUFCR (ETH_BASE + 0x01c4)


// DMA
#define ETH_DMABMR   (ETH_BASE + 0x1000)
#define ETH_DMATPDR  (ETH_BASE + 0x1004)
#define ETH_DMARPDR  (ETH_BASE + 0x1008)
#define ETH_DMARDLAR (ETH_BASE + 0x100c)
#define ETH_DMATDLAR (ETH_BASE + 0x1010)
#define ETH_DMASR    (ETH_BASE + 0x1014)

#define ETH_DMAOMR   (ETH_BASE + 0x1018)
#define ETH_DMAIER   (ETH_BASE + 0x101c)

#define ETH_DMAMFBOCR (ETH_BASE + 0x1020)
#define ETH_DMACHTDR  (ETH_BASE + 0x1048)
#define ETH_DMACHRDR  (ETH_BASE + 0x104c)
#define ETH_DMACHTBAR (ETH_BASE + 0x1050)
#define ETH_DMACHRBAR (ETH_BASE + 0x1054)

#define SYSCFG_PMC 0x40013804

typedef struct {
  uint32_t w0;
  uint32_t w1;
  union {
    uint32_t w2;
    void *buf;
  };
  union {
    uint32_t w3;
    void *next;
  };
} desc_t;

#define ETH_RDES0_OWN (1 << 31)
#define ETH_RDES0_FD  (1 << 9)  // First descriptor in packet
#define ETH_RDES0_LD  (1 << 8)  // Last descriptor in packet

#define ETH_RDES1_RCH (1 << 14)

#define ETH_TDES0_OWN (1 << 31)
#define ETH_TDES0_IC  (1 << 30)
#define ETH_TDES0_FS  (1 << 28)
#define ETH_TDES0_LS  (1 << 29)
#define ETH_TDES0_CIC (1 << 22)
#define ETH_TDES0_TCH (1 << 20)


typedef struct stm32f4_eth {
  ether_netif_t se_eni;

  struct pbuf_queue se_rx_scatter_queue;
  size_t se_rx_scatter_length;

  desc_t *se_txring;
  desc_t *se_rxring;

  void *se_tx_pbuf_data[ETH_TX_RING_SIZE];

  uint8_t se_next_rx;

  uint8_t se_tx_rdptr; // Where DMA currently is
  uint8_t se_tx_wrptr; // Where we will write next TX desc

  uint8_t se_phyaddr;

  timer_t se_periodic;

} stm32f4_eth_t;


const char *dmasr_bitnames =
  "SR\0"
  "TPSS\0"
  "TBUS\0"
  "TJTS\0"
  "ROS\0"
  "TUS\0"
  "RS\0"
  "RBUS\0"
  "RPSS\0"
  "RWTS\0"
  "ETS\0"
  "res\0"
  "res\0"
  "FBES\0"
  "ERS\0"
  "AIS\0"
  "NIS\0";

const char *txfiforxstatus =
  "Idle\0"
  "Read\0"
  "WaitMac\0"
  "Write\0";

const char *mactxfcstatus =
  "Idle\0"
  "IFG\0"
  "TXPause\0"
  "TXFrame\0";

static void
stm32f4_eth_print_info(struct device *dev, struct stream *st)
{
  stm32f4_eth_t *se = (stm32f4_eth_t *)dev;
  ether_print(&se->se_eni, st);

  uint32_t dmasr = reg_rd(ETH_DMASR);
  stprintf(st, "\tDMASR: TPS: %d  ", (dmasr >> 20) & 7);
  stprintf(st, "RPS: %d  ", (dmasr >> 17) & 7);
  stprintflags(st, dmasr_bitnames, dmasr & 0x1ffff, " ");
  stprintf(st, "\n");

  uint32_t macdbg = reg_rd(ETH_MACDBGR);
  stprintf(st, "\tMACDBG: 0x%08x\n", macdbg);
  stprintf(st, "\tMAC/TX: %s%s%s%s %s%s%s\n",
           macdbg & (1 << 25) ? "TFF " : "",
           macdbg & (1 << 24) ? "TFNE " : "",
           macdbg & (1 << 22) ? "TFWA " : "",
           strtbl(txfiforxstatus, (macdbg >> 20) & 3),
           macdbg & (1 << 19) ? "MTP " : "",
           strtbl(mactxfcstatus, (macdbg >> 17) & 3),
           macdbg & (1 << 16) ? "MMTEA " : "");

}

static const device_class_t stm32f4_eth_device_class = {
  .dc_print_info = stm32f4_eth_print_info,
};



static stm32f4_eth_t eth;


static void
rx_desc_give(desc_t *rx, void *buf)
{
  rx->buf = buf + DMA_BUFFER_PAD;
  rx->w0 = ETH_RDES0_OWN;
}


static uint16_t
mii_read(void *arg, uint16_t reg)
{
  stm32f4_eth_t *se = arg;
  int phyaddr = se->se_phyaddr;

  reg_wr(ETH_MACMIIAR,
         (phyaddr << 11) |
         (reg << 6) |
         (0 << 1) |  // Read
         (1 << 0));

  while(reg_rd(ETH_MACMIIAR) & 1) {
    if(can_sleep()) {
      usleep(10);
    }
  }
  return reg_rd(ETH_MACMIIDR) & 0xffff;
}


static void
mii_write(void *arg, uint16_t reg, uint16_t value)
{
  stm32f4_eth_t *se = arg;
  int phyaddr = se->se_phyaddr;

  reg_wr(ETH_MACMIIDR, value);
  reg_wr(ETH_MACMIIAR,
         (phyaddr << 11) |
         (reg << 6) |
         (1 << 1) |  // Write
         (1 << 0));
  while(reg_rd(ETH_MACMIIAR) & 1) {
    if(can_sleep()) {
      usleep(10);
    }
  }
}

static void *__attribute__((noreturn))
stm32f4_phy_thread(void *arg)
{
  stm32f4_eth_t *se = arg;
  int current_up = 0;

  while(1) {
    mii_read(se, 1);
    int n = mii_read(se, 1);
    int up = !!(n & 4);

    if(!current_up && up) {

      // FIXME: Configure correct speed and duplex in MAC

      current_up = 1;
      evlog(LOG_INFO, "eth: Link status: %s (0x%x)", "UP", n);
      net_task_raise(&se->se_eni.eni_ni.ni_task, NETIF_TASK_STATUS_UP);
    } else if(current_up && !up) {
      current_up = 0;
      evlog(LOG_INFO, "eth: Link status: %s", "DOWN");
      net_task_raise(&se->se_eni.eni_ni.ni_task, NETIF_TASK_STATUS_DOWN);
    }
    usleep(100000);
  }
}


static void
eth_irq_rx(stm32f4_eth_t *se)
{
  while(1) {
    desc_t *rx = se->se_rxring + (se->se_next_rx & ETH_RX_RING_MASK);
    const uint32_t w0 = rx->w0;
    if(w0 & ETH_RDES0_OWN)
      break;
    const int len = (w0 >> 16) & 0x3fff;
    if(w0 & ETH_RDES0_FD) {
      pbuf_t *pb = STAILQ_FIRST(&se->se_rx_scatter_queue);
      if(pb != NULL)
        pbuf_free_irq_blocked(pb);
      se->se_rx_scatter_length = 0;
      STAILQ_INIT(&se->se_rx_scatter_queue);
    }

    void *buf = rx->buf;
    pbuf_t *pb = pbuf_get(0);
    if(pb != NULL) {
      void *nextbuf = pbuf_data_get(0);

      if(nextbuf) {
        int flags = 0;

        if(w0 & ETH_RDES0_FD) {
          flags |= PBUF_SOP;
          pb->pb_offset = 2;
        } else {
          pb->pb_offset = 0;
        }
        if(w0 & ETH_RDES0_LD) {
          flags |= PBUF_EOP;
        }

        pb->pb_data = buf - DMA_BUFFER_PAD;
        pb->pb_flags = flags;

        if(flags == (PBUF_SOP | PBUF_EOP)) {
          se->se_eni.eni_stats.rx_byte += pb->pb_pktlen;
          se->se_eni.eni_stats.rx_pkt++;

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
            se->se_eni.eni_stats.rx_byte += pb->pb_pktlen;
            se->se_eni.eni_stats.rx_pkt++;

            STAILQ_CONCAT(&se->se_eni.eni_ni.ni_rx_queue,
                          &se->se_rx_scatter_queue);
            netif_wakeup(&se->se_eni.eni_ni);
          } else {
            se->se_rx_scatter_length = len;
          }
        }
        rx->buf = nextbuf + 2;
      } else {
        se->se_eni.eni_stats.rx_sw_qdrop++;
        pbuf_put(pb);
      }
    } else {
      se->se_eni.eni_stats.rx_sw_qdrop++;
    }
    rx->w0 = ETH_RDES0_OWN;
    se->se_next_rx++;
  }
  asm volatile ("dsb;isb");
  reg_wr(ETH_DMARPDR, 0);
}


static void
eth_irq_tx(stm32f4_eth_t *se)
{
  while(se->se_tx_rdptr != se->se_tx_wrptr) {
    desc_t *tx = se->se_txring + (se->se_tx_rdptr & ETH_TX_RING_MASK);

    const uint32_t w0 = tx->w0;
    if(w0 & ETH_TDES0_OWN)
      break;
    pbuf_data_put(se->se_tx_pbuf_data[se->se_tx_rdptr & ETH_TX_RING_MASK]);
    se->se_tx_rdptr++;

    if(w0 & ETH_TDES0_FS) {
      se->se_eni.eni_stats.tx_pkt++;
    }
    se->se_eni.eni_stats.tx_byte += tx->w1;
  }
}


void
irq_61(void)
{
  stm32f4_eth_t *se = &eth;

  uint32_t dmasr = reg_rd(ETH_DMASR);
  if(dmasr & (1 << 6))
    eth_irq_rx(se);
  if(dmasr & (1 << 0))
    eth_irq_tx(se);

  reg_wr(ETH_DMASR, dmasr);
}



static error_t
stm32f4_eth_output(struct ether_netif *eni, pbuf_t *pkt, int flags)
{
  stm32f4_eth_t *se = (stm32f4_eth_t *)eni;
  pbuf_t *pb;
  size_t count = 0;

  for(pb = pkt; pb != NULL; pb = pb->pb_next) {
    count++;
  }

  int q = irq_forbid(IRQ_LEVEL_NET);
  int wrptr = se->se_tx_wrptr;
  uint8_t qlen = (wrptr - se->se_tx_rdptr) & 0xff;

  if(qlen + count >= ETH_TX_RING_SIZE) {
    pbuf_free_irq_blocked(pkt);
    eni->eni_stats.tx_qdrop++;
    irq_permit(q);
    return ERR_QUEUE_FULL;
  }

  for(pb = pkt; pb != NULL; pb = pb->pb_next) {

    desc_t *tx = se->se_txring + (wrptr & ETH_TX_RING_MASK);
    se->se_tx_pbuf_data[wrptr & ETH_TX_RING_MASK] = pb->pb_data;

    tx->buf = pb->pb_data + pb->pb_offset;
    tx->w1 = pb->pb_buflen;

    uint32_t w0 = ETH_TDES0_OWN  | ETH_TDES0_TCH;
    w0 |= 3 << 22;

    if(pb->pb_flags & PBUF_SOP) {
      w0 |= ETH_TDES0_FS;
    }
    if(pb->pb_flags & PBUF_EOP) {
      w0 |= ETH_TDES0_LS | ETH_TDES0_IC;
    }
    tx->w0 = w0;

    wrptr++;
  }

  //  desc_t *tx = se->se_txring + (wrptr & ETH_TX_RING_MASK);
  asm volatile ("dsb;isb");
  reg_wr(ETH_DMATPDR, 0);

  se->se_tx_wrptr = wrptr;
  for(; pkt != NULL; pkt = pb) {
    pb = STAILQ_NEXT(pkt, pb_link);
    pbuf_put(pkt); // Free header, data will be free'd after TX is done
  }

  irq_permit(q);
  return 0;
}


static void
stm32f4_periodic(void *opaque, uint64_t now)
{
  stm32f4_eth_t *se = opaque;

  // These counters auto-clear on read

  uint32_t crcerr = reg_rd(ETH_MMCRFCECR);
  if(crcerr) {
    se->se_eni.eni_stats.rx_crc += crcerr;
  }

  uint32_t dmaerr = reg_rd(ETH_DMAMFBOCR);
  uint32_t mfc = dmaerr & 0xffff;
  if(mfc) {
    se->se_eni.eni_stats.rx_hw_qdrop += mfc;
  }

  uint32_t mfa = (dmaerr >> 17) & 0x7ff;
  if(mfa) {
    se->se_eni.eni_stats.rx_other_err += mfa;
  }

  net_timer_arm(&se->se_periodic, now + 100000);
}

static const ethphy_reg_io_t stm32f4_eth_mdio = {
  .read = mii_read,
  .write = mii_write
};

void
stm32f4_eth_init(gpio_t phyrst, const uint8_t *gpios, size_t gpio_count,
                 const ethphy_driver_t *ethphy, int phy_addr,
                 ethphy_mode_t mode)
{
  stm32f4_eth_t *se = &eth;

  clk_enable(CLK_SYSCFG);

  if(mode == ETHPHY_MODE_RMII) {
    reg_set_bit(SYSCFG_PMC, 23); // Enable RMII mode
  } else {
    reg_clr_bit(SYSCFG_PMC, 23); // Enable MII mode
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

  clk_enable(CLK_ETH);
  clk_enable(CLK_ETHTX);
  clk_enable(CLK_ETHRX);

  reset_peripheral(CLK_ETH);

  se->se_phyaddr = phy_addr;

  if(ethphy) {
    error_t err = ethphy->init(mode, &stm32f4_eth_mdio, se);
    if(err) {
      evlog(LOG_ERR, "stm32f4: PHY init failed");
      return;
    }
  }

  reg_set_bit(ETH_DMABMR, 0);

  int i = 0;
  while(reg_rd(ETH_DMABMR) & 1) {
    i++;
    if(i == 1000000) {
      evlog(LOG_ERR, "Ethernet MAC failed to initialize, no clock?");
      return;
    }
  }
  udelay(10);

  reg_set_bit(ETH_MACCR, 13); // Receive-Own-Disable
  reg_set_bit(ETH_MACCR, 25); // Strip CRC

  reg_wr(ETH_MACFFR, (1 << 31)); // Receive all

  reg_wr(ETH_DMAOMR,
         (1 << 25) | // RX store-and-forward
         (1 << 21) | // TX store-and-forward
         0);

  reg_wr(ETH_DMABMR,
         (1 << 25));


  se->se_eni.eni_addr[0] = 0x06;
  se->se_eni.eni_addr[1] = 0x00;

  const struct serial_number sn = sys_get_serial_number();
  uint32_t uuidcrc = crc32(0, sn.data, sn.len);
  memcpy(&se->se_eni.eni_addr[2], &uuidcrc, 4);

  STAILQ_INIT(&se->se_rx_scatter_queue);

  se->se_txring = xalloc(sizeof(desc_t) * ETH_TX_RING_SIZE, 0, MEM_TYPE_DMA);
  se->se_rxring = xalloc(sizeof(desc_t) * ETH_RX_RING_SIZE, 0, MEM_TYPE_DMA);

  for(int i = 0; i < ETH_RX_RING_SIZE; i++) {
    void *buf = pbuf_data_get(0);
    if(buf == NULL)
      break;
    desc_t *rx = se->se_rxring + i;
    memset(rx, 0, sizeof(desc_t));

    rx->w1 = ETH_RDES1_RCH | (PBUF_DATA_SIZE - DMA_BUFFER_PAD);
    rx->next = se->se_rxring + ((i + 1) & (ETH_RX_RING_SIZE - 1));
    rx_desc_give(rx, buf);
  }

  for(int i = 0; i < ETH_TX_RING_SIZE; i++) {
    desc_t *tx = se->se_txring + i;
    memset(tx, 0, sizeof(desc_t));
    tx->next = se->se_txring + ((i + 1) & (ETH_TX_RING_SIZE - 1));
  }

  reg_wr(ETH_DMARDLAR, (uint32_t)se->se_rxring);
  reg_wr(ETH_DMATDLAR, (uint32_t)se->se_txring);

  reg_wr(ETH_MACIMR, (1 << 9) | (1 << 3));

  reg_wr(ETH_DMAIER, (1 << 16) | (1 << 6) | (1 << 0));

  reg_set_bit(ETH_DMAOMR, 13); // Start TX
  reg_set_bit(ETH_DMAOMR, 1);  // Start RX

  reg_set_bit(ETH_MACCR, 14); // 100Mbps
  reg_set_bit(ETH_MACCR, 11); // FDX


  reg_set_bit(ETH_MMCCR, 2); // Clear on read

  se->se_eni.eni_output = stm32f4_eth_output;

  se->se_periodic.t_cb = stm32f4_periodic;
  se->se_periodic.t_opaque = se;

  se->se_eni.eni_ni.ni_flags |=
    NETIF_F_TX_IPV4_CKSUM_OFFLOAD |
    NETIF_F_TX_ICMP_CKSUM_OFFLOAD |
    NETIF_F_TX_UDP_CKSUM_OFFLOAD |
    NETIF_F_TX_TCP_CKSUM_OFFLOAD;

  net_timer_arm(&se->se_periodic, clock_get() + 100000);

  ether_netif_init(&se->se_eni, "eth0", &stm32f4_eth_device_class);

  irq_enable(61, IRQ_LEVEL_NET);

  thread_create(stm32f4_phy_thread, se, 512, "phy",
                TASK_NO_FPU | TASK_NO_DMA_STACK, 4);

  reg_set_bit(ETH_MACCR, 3);   // TE
  reg_set_bit(ETH_MACCR, 2);   // RE
}
