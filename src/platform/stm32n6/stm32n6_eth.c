#include <assert.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <mios/io.h>
#include <mios/eventlog.h>
#include <mios/sys.h>
#include <mios/driver.h>
#include <malloc.h>
#include <unistd.h>

#include <net/pbuf.h>
#include <net/ether.h>

#include <util/crc32.h>

#include "irq.h"
#include "cache.h"
#include "stm32n6_clk.h"
#include "stm32n6_eth.h"

#define MAC_NAME "stm32n6-eth"

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
// Write-back format: bit 30 = CTXT (context descriptor with timestamp)
#define ETH_RDES3_CTX           0x40000000

#define ETH_TDES3_OWN           0x80000000
#define ETH_TDES3_FD            0x20000000
#define ETH_TDES3_LD            0x10000000
// CIC[1:0] in TDES3 bits[17:16]: 11 = full IP+TCP/UDP/ICMP checksum
// insertion including pseudo-header (RFC compliant). Read by the MAC
// from the FD descriptor of each packet; ignored on mid/LD descriptors.
#define ETH_TDES3_CIC_FULL      0x00030000

#define ETH_BASE 0x48036000

#define ETH_MACCR   (ETH_BASE + 0x0)
#define ETH_MACECR  (ETH_BASE + 0x4)
#define ETH_MACPFR  (ETH_BASE + 0x8)
#define ETH_MACWTR  (ETH_BASE + 0xc)

#define ETH_MACQTXFCR  (ETH_BASE + 0x70)
#define ETH_MACRXFCR   (ETH_BASE + 0x90)
#define ETH_MACISR     (ETH_BASE + 0xb0)
#define ETH_MACIER     (ETH_BASE + 0xb4)

#define ETH_MACVR      (ETH_BASE + 0x110)

#define ETH_MACMDIOAR  (ETH_BASE + 0x200)
#define ETH_MACMDIODR  (ETH_BASE + 0x204)

#define ETH_MACAHR(x)  (ETH_BASE + 0x300 + 8 * (x))
#define ETH_MACALR(x)  (ETH_BASE + 0x304 + 8 * (x))

#define ETH_MMC_CONTROL       (ETH_BASE + 0x700)
#define ETH_MMC_RX_INTR_MASK  (ETH_BASE + 0x70c)
#define ETH_MMC_TX_INTR_MASK  (ETH_BASE + 0x710)

#define ETH_MTLOMR       (ETH_BASE + 0xc00)
#define ETH_MTLISR       (ETH_BASE + 0xc20)

// Queue 0 MTL registers (N6 multi-queue: must enable TXQEN)
#define ETH_MTL_TXQ0OMR  (ETH_BASE + 0xd00)
#define ETH_MTL_RXQ0OMR  (ETH_BASE + 0xd30)

#define ETH_DMAMR        (ETH_BASE + 0x1000)
#define ETH_DMASBMR      (ETH_BASE + 0x1004)
#define ETH_DMAISR       (ETH_BASE + 0x1008)

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

// PTP Timestamp registers
#define ETH_MACTSCR      (ETH_BASE + 0xb00)
#define ETH_MACSSIR      (ETH_BASE + 0xb04)
#define ETH_MACSTSR      (ETH_BASE + 0xb08)
#define ETH_MACSTNR      (ETH_BASE + 0xb0c)
#define ETH_MACSTSUR     (ETH_BASE + 0xb10)
#define ETH_MACSTNUR     (ETH_BASE + 0xb14)
#define ETH_MACTSAR      (ETH_BASE + 0xb18)
#define ETH_MACPPSCR     (ETH_BASE + 0xb70)


#define ETH_TX_RING_SIZE 64
#define ETH_TX_RING_MASK (ETH_TX_RING_SIZE - 1)

#define ETH_RX_RING_SIZE 16
#define ETH_RX_RING_MASK (ETH_RX_RING_SIZE - 1)

typedef struct stm32n6_eth {
  ether_netif_t se_eni;

  struct pbuf_queue se_rx_scatter_queue;
  int se_rx_scatter_length;

  desc_t *se_txring;
  desc_t *se_rxring;

  void *se_tx_pbuf_data[ETH_TX_RING_SIZE];
  void *se_tx_pbuf[ETH_TX_RING_SIZE];
  void *se_rx_pbuf_data[ETH_RX_RING_SIZE];

  uint8_t se_next_rx;

  uint8_t se_tx_rdptr;
  uint8_t se_tx_wrptr;

  uint8_t se_phyaddr;

  timer_t se_periodic;

#ifdef ENABLE_NET_PTP
  uint32_t se_addend;
  int32_t se_ppb_scale_mult;
  int32_t se_ppb_scale_div;
#endif

} stm32n6_eth_t;


static stm32n6_eth_t stm32n6_eth;

static error_t stm32n6_eth_output(struct ether_netif *eni, pbuf_t *pb,
                                  pbuf_tx_cb_t *txcb, uint32_t id);
static void stm32n6_eth_irq(void *arg);
static void stm32n6_eth_periodic(void *opaque, uint64_t now);


static void
rx_desc_give(stm32n6_eth_t *se, size_t index, void *buf)
{
  assert(buf != NULL);
  se->se_rx_pbuf_data[index] = buf;

  // Discard any dirty cache lines before DMA writes to this buffer
  dcache_op(buf, PBUF_DATA_SIZE, DCACHE_INVALIDATE);

  desc_t *rx = se->se_rxring + index;
  rx->p0 = buf + DMA_BUFFER_PAD;
  rx->w3 = ETH_RDES3_OWN | ETH_RDES3_IOC | ETH_RDES3_BUF1V;
  asm volatile ("dsb");
  reg_wr(ETH_DMACRXDTPR, 0);
}


static void
stm32n6_eth_print_info(struct device *dev, struct stream *st)
{
  ether_print((ether_netif_t *)dev, st);
#ifdef ENABLE_NET_PTP
  stm32n6_eth_t *se = (stm32n6_eth_t *)dev;
  if(ptp_print_info(st, &se->se_eni)) {
    stprintf(st, "  PTP addend:0x%x seconds:%d nano:%d\n",
             se->se_addend,
             reg_rd(ETH_MACSTSR),
             reg_rd(ETH_MACSTNR));
  }
#endif
}


static uint16_t
mii_read(void *arg, uint16_t reg)
{
  stm32n6_eth_t *se = arg;

  reg_wr(ETH_MACMDIOAR,
         (se->se_phyaddr << 21) |
         (reg << 16) |
         (0 << 12) |
         (0 << 8) |
         (3 << 2) |
         (1 << 0));

  while(reg_rd(ETH_MACMDIOAR) & 1) {
    if(can_sleep())
      usleep(10);
  }
  return reg_rd(ETH_MACMDIODR) & 0xffff;
}


static void
mii_write(void *arg, uint16_t reg, uint16_t value)
{
  stm32n6_eth_t *se = arg;

  reg_wr(ETH_MACMDIODR, value);
  reg_wr(ETH_MACMDIOAR,
         (se->se_phyaddr << 21) |
         (reg << 16) |
         (1 << 2) |
         (1 << 0));
  while(reg_rd(ETH_MACMDIOAR) & 1) {
    if(can_sleep())
      usleep(10);
  }
}


static int
edc_mii_read(ether_netif_t *eni, uint16_t reg)
{
  return mii_read((stm32n6_eth_t *)eni, reg);
}

static error_t
edc_mii_write(ether_netif_t *eni, uint16_t reg, uint16_t value)
{
  mii_write((stm32n6_eth_t *)eni, reg, value);
  return 0;
}

static void
stm32n6_eth_set_link_params(ether_netif_t *eni, int speed, int full_duplex)
{
  if(speed == 1000) {
    reg_clr_bit(ETH_MACCR, 15); // PS=0: GMII/RGMII
    reg_clr_bit(ETH_MACCR, 14); // FES=0
  } else if(speed == 100) {
    reg_set_bit(ETH_MACCR, 15); // PS=1: MII/RMII
    reg_set_bit(ETH_MACCR, 14); // FES=1: 100Mbit
  } else {
    reg_set_bit(ETH_MACCR, 15); // PS=1: MII/RMII
    reg_clr_bit(ETH_MACCR, 14); // FES=0: 10Mbit
  }

  if(full_duplex)
    reg_set_bit(ETH_MACCR, 13);
  else
    reg_clr_bit(ETH_MACCR, 13);
}

static const ethmac_device_class_t stm32n6_eth_device_class = {
  .dc = {
    .dc_class_name = MAC_NAME,
    .dc_print_info = stm32n6_eth_print_info,
  },
  .edc_mii_read = edc_mii_read,
  .edc_mii_write = edc_mii_write,
  .edc_set_link_params = stm32n6_eth_set_link_params,
};


#ifdef ENABLE_NET_PTP

static void
stm32n6_clock_set_time(clock_realtime_t *clk, int64_t nsec)
{
  reg_wr(ETH_MACSTSUR, nsec / 1000000000);
  reg_wr(ETH_MACSTNUR, nsec % 1000000000);
  reg_set_bit(ETH_MACTSCR, 2);
  while(reg_get_bit(ETH_MACTSCR, 2));
}

static int64_t
stm32n6_clock_get_time(clock_realtime_t *clk)
{
  while(1) {
    uint32_t s = reg_rd(ETH_MACSTSR);
    uint32_t ns = reg_rd(ETH_MACSTNR);
    if(s == reg_rd(ETH_MACSTSR))
      return (int64_t)s * 1000000000 + ns;
  }
}

static void
stm32n6_clock_adj_time(clock_realtime_t *clk, int32_t ppb)
{
  stm32n6_eth_t *se = &stm32n6_eth;

  int32_t addend_adjustment =
    ((int64_t)ppb * se->se_ppb_scale_mult) / se->se_ppb_scale_div;

  uint32_t final_addend = se->se_addend + addend_adjustment;
  reg_wr(ETH_MACTSAR, final_addend);
  reg_set_bit(ETH_MACTSCR, 5);
}

static const clock_realtime_class_t stm32n6_clock_realtime_class = {
  .name = MAC_NAME,
  .set_time = stm32n6_clock_set_time,
  .get_time = stm32n6_clock_get_time,
  .adj_time = stm32n6_clock_adj_time,
};

static void
stm32n6_ptp_init(stm32n6_eth_t *se)
{
  // Select HSE as PTP reference clock (ETH1PTPSEL = 3 = hse_ck)
  reg_set_bits(RCC_CCIPR2, 0, 2, 3);

  const unsigned int ptp_freq = stm32n6_hse_freq;

  uint32_t ssinc = (1000000000 / ptp_freq) + 1;

  uint64_t denominator = (uint64_t)ptp_freq * ssinc;
  se->se_addend = (1000000000ULL << 32) / denominator;

  se->se_ppb_scale_mult = se->se_addend / 1000;
  se->se_ppb_scale_div = 1000000;

  reg_wr(ETH_MACTSCR,
         (1 << 11) | // PTPv2 ethernet
         (1 << 10) | // Enable timestamping for PTPv2
         (1 << 9)  | // Rollover ns at 999999999
         (1 << 1)  | // Fine mode
         (1 << 0)  | // Enable
         0);

  reg_set_bits(ETH_MACSSIR, 16, 8, ssinc);

  reg_wr(ETH_MACSTSUR, 0);
  reg_wr(ETH_MACSTNUR, 0);
  reg_set_bit(ETH_MACTSCR, 2);

  reg_wr(ETH_MACTSAR, se->se_addend);
  reg_set_bit(ETH_MACTSCR, 5);
  while(reg_get_bit(ETH_MACTSCR, 5));

  se->se_eni.eni_ptp.pes_clock.clk_class = &stm32n6_clock_realtime_class;
  clock_servo_init(&se->se_eni.eni_ptp.pes_servo,
                   &se->se_eni.eni_ptp.pes_clock);

  reg_wr(ETH_MACPPSCR, 1);
}

#endif


__attribute__((noreturn))
static void
stm32n6_thread(stm32n6_eth_t *se, gpio_t phyrst, ethphy_mode_t miimode)
{
  device_t *self = &se->se_eni.eni_ni.ni_dev;

  if(phyrst != GPIO_UNUSED) {
    gpio_conf_output(phyrst, GPIO_PUSH_PULL,
                     GPIO_SPEED_LOW, GPIO_PULL_NONE);
    gpio_set_output(phyrst, 0);
    usleep(100000);
    gpio_set_output(phyrst, 1);
    usleep(100000);
  }

  // Select PHY interface in RCC_CCIPR2 before enabling clocks
  // ETH1SEL[18:16]: 000=MII, 001=RGMII, 100=RMII
  reg_set_bits(RCC_CCIPR2, 16, 3, (miimode == ETHPHY_MODE_RGMII) ? 1 : 0);

  clk_enable(CLK_ETH1MACEN);
  clk_enable(CLK_ETH1TXEN);
  clk_enable(CLK_ETH1RXEN);
  clk_enable(CLK_ETH1EN);

  rst_assert(CLK_ETH1EN);
  rst_deassert(CLK_ETH1EN);

  ethphy_init_t *init = driver_probe(DRIVER_TYPE_ETHPHY, self);

  if(init == NULL) {
    evlog(LOG_ERR, "%s: No PHY driver found", self->d_name);
    thread_exit(0);
  }

  se->se_eni.eni_phy = init(&se->se_eni, miimode,
                            ETHPHY_DELAY_TX | ETHPHY_DELAY_RX);

  // PHY must be providing clocks before MAC soft reset can complete
  usleep(10000);

  // Soft reset
  reg_set_bit(ETH_DMAMR, 0);

  int i = 0;
  while(reg_rd(ETH_DMAMR) & 1) {
    i++;
    usleep(10);
    if(i == 100) {
      evlog(LOG_ERR, "Ethernet MAC failed to initialize, no clock?");
      thread_exit(0);
    }
  }
  usleep(10);

#ifdef ENABLE_NET_PTP
  stm32n6_ptp_init(se);
#endif

  // MTL TX queue 0: enable (TXQEN=2), store-and-forward, queue size
  reg_wr(ETH_MTL_TXQ0OMR, (7 << 16) | (2 << 2) | (1 << 1));
  // MTL RX queue 0: store-and-forward, queue size
  reg_wr(ETH_MTL_RXQ0OMR, (7 << 20) | (1 << 5));

  // Enable RX queue 0 in MAC (RXQ0EN=2: enabled for DCB/generic)
  reg_set_bits(ETH_BASE + 0xa0, 0, 2, 2);  // MACRXQC0R

  reg_wr(ETH_MACPFR, 1 << 31);  // Receive ALL

  // DMA channel config
  reg_wr(ETH_DMACTXCR, (32 << 16));  // TXPBL=32
  reg_wr(ETH_DMACRXCR, (32 << 16) |
         ((PBUF_DATA_SIZE - DMA_BUFFER_PAD) << 1));  // RXPBL=32 + RBSZ

  reg_wr(ETH_DMACTXDLAR, (uint32_t)se->se_txring);
  reg_wr(ETH_DMACTXRLR,  ETH_TX_RING_SIZE - 1);

  reg_wr(ETH_DMACRXDLAR, (uint32_t)se->se_rxring);
  reg_wr(ETH_DMACRXRLR,  ETH_RX_RING_SIZE - 1);

  reg_wr(ETH_DMACIER, (1 << 15) | (1 << 6) | (1 << 2) | (1 << 0));

  reg_set_bit(ETH_MACCR, 21); // CST: Strip CRC

  reg_set_bit(ETH_MACCR, 0); // Enable RX
  reg_set_bit(ETH_MACCR, 1); // Enable TX

  // MMC: clear counters on read
  reg_set_bit(ETH_MMC_CONTROL, 2);

  // Start DMA TX and RX
  reg_set_bit(ETH_DMACTXCR, 0);
  reg_set_bit(ETH_DMACRXCR, 0);

  se->se_eni.eni_output = stm32n6_eth_output;

  se->se_eni.eni_ni.ni_flags |=
    NETIF_F_TX_IPV4_CKSUM_OFFLOAD |
    NETIF_F_TX_ICMP_CKSUM_OFFLOAD |
    NETIF_F_TX_UDP_CKSUM_OFFLOAD |
    NETIF_F_TX_TCP_CKSUM_OFFLOAD;

  se->se_periodic.t_cb = stm32n6_eth_periodic;
  se->se_periodic.t_opaque = se;
  net_timer_arm(&se->se_periodic, clock_get() + 100000);

  ether_netif_attach(&se->se_eni);

  irq_enable_fn_arg(179, IRQ_LEVEL_NET, stm32n6_eth_irq, se);

  ethphy_link_poll(&se->se_eni);
}


static void
handle_irq_rx(stm32n6_eth_t *se)
{
  while(1) {
    size_t rx_idx = se->se_next_rx & ETH_RX_RING_MASK;
    desc_t *rx = se->se_rxring + rx_idx;
    const uint32_t w3 = rx->w3;
    if(w3 & ETH_RDES3_OWN)
      break;
    const int len = w3 & 0x7fff;

    if((w3 & (ETH_RDES3_FD | ETH_RDES3_CTX)) == ETH_RDES3_FD) {
      pbuf_t *pb = STAILQ_FIRST(&se->se_rx_scatter_queue);
      if(pb != NULL) {
        pbuf_free_irq_blocked(pb);
        STAILQ_INIT(&se->se_rx_scatter_queue);
      }
      se->se_rx_scatter_length = 0;
    }

    void *buf = se->se_rx_pbuf_data[rx_idx];
    assert(buf != NULL);
    // Invalidate cache to discard any speculatively prefetched stale data
    dcache_op(buf, PBUF_DATA_SIZE, DCACHE_INVALIDATE);
    pbuf_t *pb = pbuf_get(0);
    if(pb != NULL) {
      void *nextbuf = pbuf_data_get(0);
      if(nextbuf != NULL) {

        if(unlikely(w3 & ETH_RDES3_CTX)) {

          // Context descriptor — carries RX timestamp
          pbuf_t *pb2 = STAILQ_FIRST(&se->se_rx_scatter_queue);
          if(likely(pb2 != NULL)) {
            pb->pb_flags = PBUF_TIMESTAMP | PBUF_SOP;
            pb->pb_pktlen = pb2->pb_pktlen + sizeof(pbuf_timestamp_t);
            pb->pb_offset = 0;
            pb->pb_buflen = sizeof(pbuf_timestamp_t);
            pb->pb_data = buf;

            pbuf_timestamp_t *pt = buf;
            pt->pt_cb = NULL;
            pt->pt_seconds = rx->w1;
            pt->pt_nanoseconds = rx->w0;

            pb2->pb_flags &= ~PBUF_SOP;

            se->se_eni.eni_stats.rx_pkt++;
            se->se_eni.eni_stats.rx_byte += pb2->pb_pktlen;

            STAILQ_INSERT_TAIL(&se->se_eni.eni_ni.ni_rx_queue, pb, pb_link);
            STAILQ_CONCAT(&se->se_eni.eni_ni.ni_rx_queue,
                          &se->se_rx_scatter_queue);
            netif_wakeup(&se->se_eni.eni_ni);
          } else {
            pbuf_put(pb);
            pbuf_data_put(buf);
          }

        } else {

          int flags = 0;
          int tsa = 0;

          if(w3 & ETH_RDES3_FD) {
            flags |= PBUF_SOP;
            pb->pb_offset = 2;
          } else {
            pb->pb_offset = 0;
          }
          if(w3 & ETH_RDES3_LD) {
            flags |= PBUF_EOP;
            if(w3 & (1 << 26)) {    // RS1V: RDES1 valid
              if(rx->w1 & (1 << 14)) // TSA: timestamp available
                tsa = 1;
            }
          }

          pb->pb_data = buf;
          pb->pb_flags = flags;

          if((flags == (PBUF_SOP | PBUF_EOP)) && likely(!tsa)) {
            pb->pb_buflen = len;
            pb->pb_pktlen = len;
            se->se_eni.eni_stats.rx_pkt++;
            se->se_eni.eni_stats.rx_byte += len;
            STAILQ_INSERT_TAIL(&se->se_eni.eni_ni.ni_rx_queue, pb, pb_link);
            netif_wakeup(&se->se_eni.eni_ni);
          } else {
            STAILQ_INSERT_TAIL(&se->se_rx_scatter_queue, pb, pb_link);

            pb->pb_buflen = len - se->se_rx_scatter_length;

            if(flags & PBUF_EOP) {
              pbuf_t *first = STAILQ_FIRST(&se->se_rx_scatter_queue);
              first->pb_pktlen = len;

              if(likely(!tsa)) {
                se->se_eni.eni_stats.rx_pkt++;
                se->se_eni.eni_stats.rx_byte += len;
                STAILQ_CONCAT(&se->se_eni.eni_ni.ni_rx_queue,
                              &se->se_rx_scatter_queue);
                netif_wakeup(&se->se_eni.eni_ni);
              }
            } else {
              se->se_rx_scatter_length = len;
            }
          }
        }
        buf = nextbuf;
      } else {
        se->se_eni.eni_stats.rx_sw_qdrop++;
        pbuf_put(pb);
      }
    }
    rx_desc_give(se, rx_idx, buf);
    se->se_next_rx++;
  }
}


static void
handle_irq_tx(stm32n6_eth_t *se)
{
  while(se->se_tx_rdptr != se->se_tx_wrptr) {
    const size_t rdptr = se->se_tx_rdptr & ETH_TX_RING_MASK;
    desc_t *tx = se->se_txring + rdptr;

    const uint32_t w3 = tx->w3;
    if(w3 & ETH_TDES3_OWN)
      break;
    if(w3 & ETH_TDES3_FD) {
      se->se_eni.eni_stats.tx_pkt++;
      se->se_eni.eni_stats.tx_byte += tx->w2 & 0x3fff;
    }

    pbuf_t *pb = se->se_tx_pbuf[rdptr];
    if(pb != NULL) {
      // PTP TX timestamp — deliver back through RX queue
      pbuf_timestamp_t *s = (pbuf_timestamp_t *)se->se_tx_pbuf[rdptr];
      pbuf_timestamp_t *pt = se->se_tx_pbuf_data[rdptr];
      pt->pt_cb = s->pt_cb;
      pt->pt_id = s->pt_id;
      pt->pt_seconds = tx->w1;
      pt->pt_nanoseconds = tx->w0;

      pb = (pbuf_t *)s;
      pb->pb_flags = PBUF_SOP | PBUF_EOP | PBUF_TIMESTAMP;
      pb->pb_offset = 0;
      pb->pb_buflen = sizeof(pbuf_timestamp_t);
      pb->pb_pktlen = pb->pb_buflen;
      pb->pb_data = pt;

      STAILQ_INSERT_TAIL(&se->se_eni.eni_ni.ni_rx_queue, pb, pb_link);
      netif_wakeup(&se->se_eni.eni_ni);
    } else {
      pbuf_data_put(se->se_tx_pbuf_data[rdptr]);
    }
    se->se_tx_rdptr++;
  }
}


static error_t
stm32n6_eth_output(struct ether_netif *eni, pbuf_t *pb,
                   pbuf_tx_cb_t *txcb, uint32_t id)
{
  stm32n6_eth_t *se = (stm32n6_eth_t *)eni;
  pbuf_t *n;
  size_t count = 0;

  for(n = pb; n != NULL; n = n->pb_next)
    count++;

  int q = irq_forbid(IRQ_LEVEL_NET);
  int wrptr = se->se_tx_wrptr;
  const uint8_t qlen = (wrptr - se->se_tx_rdptr) & 0xff;

  if(qlen + count >= ETH_TX_RING_SIZE) {
    pbuf_free_irq_blocked(pb);
    eni->eni_stats.tx_qdrop++;
    irq_permit(q);
    return ERR_QUEUE_FULL;
  }

  for(; pb != NULL; pb = n) {
    n = STAILQ_NEXT(pb, pb_link);

    desc_t *tx = se->se_txring + (wrptr & ETH_TX_RING_MASK);
    se->se_tx_pbuf_data[wrptr & ETH_TX_RING_MASK] = pb->pb_data;

    // Clean cache so DMA sees the TX data
    dcache_op(pb->pb_data, PBUF_DATA_SIZE, DCACHE_CLEAN);

    tx->p0 = pb->pb_data + pb->pb_offset;
    tx->w1 = 0;

    uint32_t w2 = (1 << 31) | pb->pb_buflen;
    uint32_t w3 = ETH_TDES3_OWN | ETH_TDES3_CIC_FULL | pb->pb_pktlen;

    if(pb->pb_flags & PBUF_SOP) {
      w3 |= ETH_TDES3_FD;
      if(unlikely(txcb != NULL))
        w2 |= (1 << 30); // TTSE: TX timestamp enable
    }

    tx->w2 = w2;

    if(pb->pb_flags & PBUF_EOP) {
      w3 |= ETH_TDES3_LD;
      if(unlikely(txcb != NULL)) {
        // Reuse pbuf for TX timestamp callback
        pbuf_timestamp_t *pt = (pbuf_timestamp_t *)pb;
        pt->pt_cb = txcb;
        pt->pt_id = id;
        se->se_tx_pbuf[wrptr & ETH_TX_RING_MASK] = pb;
        pb = NULL;
      }
    }

    if(pb != NULL) {
      pbuf_put(pb);
      se->se_tx_pbuf[wrptr & ETH_TX_RING_MASK] = NULL;
    }

    tx->w3 = w3;
    wrptr++;
  }

  asm volatile ("dsb");
  // Clear TBU + TI status, then poke tail pointer to restart DMA
  reg_wr(ETH_DMACSR, 0x5);  // Clear TBU + TI
  reg_wr(ETH_DMACTXDTPR, 0);
  // If DMA is suspended, restart it
  if(!(reg_rd(ETH_DMACTXCR) & 1))
    reg_set_bit(ETH_DMACTXCR, 0);

  se->se_tx_wrptr = wrptr;
  irq_permit(q);
  return 0;
}


static void
stm32n6_eth_irq(void *arg)
{
  stm32n6_eth_t *se = arg;

  const uint32_t dmaisr = reg_rd(ETH_DMAISR);
  if(!(dmaisr & 1))
    return;

  const uint32_t dmacsr = reg_rd(ETH_DMACSR);

  if(dmacsr & ~0x8cc5)
    panic("%s: Unhandled dmairq 0x%x", __FUNCTION__, dmacsr & ~0x8cc5);

  if(dmacsr & 0x5)
    handle_irq_tx(se);

  if(dmacsr & 0x40)
    handle_irq_rx(se);

  if(dmacsr & 0x80)
    se->se_eni.eni_stats.rx_hw_qdrop++;

  reg_wr(ETH_DMACSR, dmacsr);
}


#define ETH_MMC_RX_CRC_ERROR  (ETH_BASE + 0x794)

static void
stm32n6_eth_periodic(void *opaque, uint64_t now)
{
  stm32n6_eth_t *se = opaque;

  // MMC counters auto-clear on read (when MCR bit 2 is set)
  uint32_t crcerr = reg_rd(ETH_MMC_RX_CRC_ERROR);
  if(crcerr)
    se->se_eni.eni_stats.rx_crc += crcerr;

  net_timer_arm(&se->se_periodic, now + 100000);
}


static void
setup_rgmii_gpios(gpio_t mdio, gpio_t mdc)
{
  // RGMII data/clock pins — fixed by STM32N6 silicon, same on all boards
  static const uint8_t af11_pins[] = {
    GPIO_PF(14),  // ETH1_RGMII_RXD0
    GPIO_PF(15),  // ETH1_RGMII_RXD1
    GPIO_PF(8),   // ETH1_RGMII_RXD2
    GPIO_PF(9),   // ETH1_RGMII_RXD3
    GPIO_PF(7),   // ETH1_RGMII_RX_CLK
    GPIO_PF(10),  // ETH1_RGMII_RX_CTL
    GPIO_PF(12),  // ETH1_RGMII_TXD0
    GPIO_PF(13),  // ETH1_RGMII_TXD1
    GPIO_PG(3),   // ETH1_RGMII_TXD2
    GPIO_PG(4),   // ETH1_RGMII_TXD3
    GPIO_PF(11),  // ETH1_RGMII_TX_CTL
    GPIO_PF(2),   // ETH1_RGMII_CLK125
  };

  for(size_t i = 0; i < sizeof(af11_pins); i++) {
    gpio_conf_af(af11_pins[i], 11, GPIO_PUSH_PULL,
                 GPIO_SPEED_VERY_HIGH, GPIO_PULL_NONE);
  }

  // MDIO and MDC — board-specific pins, both AF11
  gpio_conf_af(mdio, 11, GPIO_PUSH_PULL,
               GPIO_SPEED_VERY_HIGH, GPIO_PULL_NONE);
  gpio_conf_af(mdc, 11, GPIO_PUSH_PULL,
               GPIO_SPEED_VERY_HIGH, GPIO_PULL_NONE);

  // PF0 ETH1_RGMII_GTX_CLK is AF12, not AF11. Use medium speed for signal integrity.
  gpio_conf_af(GPIO_PF(0), 12, GPIO_PUSH_PULL,
               GPIO_SPEED_MID, GPIO_PULL_NONE);

  // PF7 (ETH1_RGMII_RX_CLK): add 500ps input delay for RGMII timing
  // DELAYR[0] bits [31:28] = delay value 0x2 (500ps)
  // ADVCFGR[0] bits [31:28] = 0x3 (DLYPATH=input, DE=enable)
  #define GPIOF_BASE (0x56020000 + 5 * 0x400)
  reg_set_bits(GPIOF_BASE + 0x40, 28, 4, 0x2);  // DELAYR[0] pin 7 = 500ps
  reg_set_bits(GPIOF_BASE + 0x48, 28, 4, 0x3);  // ADVCFGR[0] pin 7 = input delay enable
}


ether_netif_t *
stm32n6_eth_init(gpio_t phyrst, gpio_t mdio, gpio_t mdc,
                 int phy_addr, ethphy_mode_t mode)
{
  stm32n6_eth_t *se = &stm32n6_eth;

  if(mode != ETHPHY_MODE_RGMII)
    panic("stm32n6_eth: only RGMII supported");

  se->se_phyaddr = phy_addr;

  ether_netif_init(&se->se_eni, "eth0", &stm32n6_eth_device_class);

#ifdef ENABLE_NET_PTP
  // Install the clock class early so callers can take a pointer to
  // pes_clock even before the worker thread completes hardware init.
  // get_time() returns 0 until stm32n6_ptp_init() configures the
  // timestamp unit.
  se->se_eni.eni_ptp.pes_clock.clk_class = &stm32n6_clock_realtime_class;
#endif

  setup_rgmii_gpios(mdio, mdc);

  se->se_eni.eni_addr[0] = 0x02;
  se->se_eni.eni_addr[1] = 0x00;

  const struct serial_number sn = sys_get_serial_number();
  uint32_t uuidcrc = crc32(0, sn.data, sn.len);
  memcpy(&se->se_eni.eni_addr[2], &uuidcrc, 4);

  se->se_tx_wrptr = 0;
  se->se_next_rx = 0;
  STAILQ_INIT(&se->se_rx_scatter_queue);
  se->se_txring = xalloc(sizeof(desc_t) * ETH_TX_RING_SIZE, 0,
                         MEM_CLEAR | MEM_TYPE_DMA | MEM_TYPE_NO_CACHE);
  se->se_rxring = xalloc(sizeof(desc_t) * ETH_RX_RING_SIZE, 0,
                         MEM_CLEAR | MEM_TYPE_DMA | MEM_TYPE_NO_CACHE);

  for(int i = 0; i < ETH_RX_RING_SIZE; i++) {
    void *buf = pbuf_data_get(0);
    if(buf == NULL)
      panic("no pbufs");
    rx_desc_give(se, i, buf);
  }

  thread_createv(stm32n6_thread, 512, "eth", 0, 4, se, phyrst, mode);
  return &se->se_eni;
}
