#include <net/ether.h>

#include <mios/pci.h>
#include <mios/mios.h>
#include <mios/driver.h>
#include <mios/eventlog.h>

#include <stdio.h>
#include <stdlib.h>
#include <malloc.h>
#include <unistd.h>

#include "reg.h"
#include "irq.h"
#include "cache.h"
#include "barrier.h"

// https://www.iitg.ac.in/asahu/cs421/RealTek.pdf

_Static_assert(PBUF_DATA_SIZE >= 1536);

#define RTK_CMD_RESET		0x10

#define RTK_MAR0                0x0008
#define RTK_MAR4                0x000c

#define RTK_TXRING_ADDR_LO	0x0020
#define RTK_TXRING_ADDR_HI	0x0024
#define RTK_COMMAND             0x0037
#define RTK_GTXSTART		0x0038
#define RTK_IMR                 0x003c
#define RTK_IS                  0x003e
#define RTK_TXCFG               0x0040
#define RTK_RXCFG	        0x0044
#define RTK_MISSEDPKT           0x004C
#define RTK_CFG1	        0x0052
#define RTK_CPLUS_CMD		0x00E0
#define RTK_IM			0x00E2
#define RTK_RXRING_ADDR_LO	0x00E4
#define RTK_RXRING_ADDR_HI	0x00E8
#define RTK_EARLY_TX_THRESH	0x00EC
#define RTK_MISC		0x00F0

#define	RL_RXCFG_RX_ALLPHYS	0x1
#define	RL_RXCFG_RX_INDIV	0x2
#define	RL_RXCFG_RX_MULTI	0x4
#define	RL_RXCFG_RX_BROAD	0x8

#define RTK_ISR_RX_OK		0x0001
#define RTK_ISR_TX_OK		0x0004

#define RX_RING_SIZE 64
#define TX_RING_SIZE 64
#define RX_RING_MASK (RX_RING_SIZE - 1)
#define TX_RING_MASK (TX_RING_SIZE - 1)

typedef struct desc {
  uint32_t cmd_status;
  uint32_t vlanctl;
  void *buf;
} desc_t;

#define DESC_OWN            0x80000000
#define DESC_EOR            0x40000000
#define DESC_FS             0x20000000
#define DESC_LS             0x10000000

#define RDESC_MCAST         0x08000000
#define RDESC_UCAST         0x04000000
#define RDESC_BCAST         0x02000000

#define RDESC_IS_UDP        0x00040000
#define RDESC_IS_TCP        0x00020000
#define RDESC_IP_BAD_CKSUM  0x00010000
#define RDESC_UDP_BAD_CKSUM 0x00008000
#define RDESC_TCP_BAD_CKSUM 0x00004000

typedef struct rtl8168 {
  ether_netif_t eni;
  pci_dev_t *pd;
  long mmio;

  desc_t *rx_ring;
  desc_t *tx_ring;

  int next_rx;

  int tx_wrptr;
  int tx_rdptr;

  pbuf_t *tx_pbuf_data[TX_RING_SIZE];
  uint16_t tx_size[TX_RING_SIZE];

  uint32_t irq_counter;
  uint32_t irq;
  char name[16];

} rtl8168_t;


static error_t
rtl8168_eth_output(struct ether_netif *eni, pbuf_t *pkt, int flags)
{
  rtl8168_t *r = (rtl8168_t *)eni;

  pbuf_t *pb;
  size_t count = 0;

  for(pb = pkt; pb != NULL; pb = pb->pb_next) {
    count++;
  }

  int q = irq_forbid(IRQ_LEVEL_NET);
  int wrptr = r->tx_wrptr;
  int qlen = (wrptr - r->tx_rdptr) & TX_RING_MASK;

  if(qlen + count >= TX_RING_SIZE) {
    pbuf_free_irq_blocked(pkt);
    r->eni.eni_stats.tx_qdrop++;
    irq_permit(q);
    return ERR_QUEUE_FULL;
  }

  int bufidx0 = wrptr & TX_RING_MASK;

  for(pb = pkt; pb != NULL; pb = pb->pb_next) {

    int bufidx = wrptr & TX_RING_MASK;

    desc_t *tx = r->tx_ring + bufidx;

    cache_op(pb->pb_data, pb->pb_buflen, DCACHE_CLEAN | DCACHE_INVALIDATE);

    r->tx_pbuf_data[bufidx] = pb->pb_data;
    r->tx_size[bufidx] = pb->pb_buflen;

    tx->buf = pb->pb_data + pb->pb_offset;

    uint32_t cmd = pb->pb_buflen;

    if(bufidx != bufidx0)
      cmd |= DESC_OWN;

    if((wrptr & TX_RING_MASK) == TX_RING_MASK)
      cmd |= DESC_EOR;

    if(pb->pb_flags & PBUF_SOP) {
      cmd |= DESC_FS;
    }
    if(pb->pb_flags & PBUF_EOP) {
      cmd |= DESC_LS;
    }
    tx->cmd_status = cmd;
    tx->vlanctl = 0;
    wrptr++;
  }

  // Wait until the end to hand over the descriptor for the first fragment
  dmb();
  desc_t *tx = r->tx_ring + bufidx0;
  tx->cmd_status |= DESC_OWN;
  dmb();

  reg_wr8(r->mmio + RTK_GTXSTART, 0x40); // Start TX
  r->tx_wrptr = wrptr;

  for(; pkt != NULL; pkt = pb) {
    pb = STAILQ_NEXT(pkt, pb_link);
    pbuf_put(pkt); // Free header, data will be free'd after TX is done
  }

  irq_permit(q);
  return 0;
}


static void
rtl8168_print_info(struct device *dev, struct stream *st)
{
  rtl8168_t *r = (rtl8168_t *)dev;
  ether_print((ether_netif_t *)dev, st);
  stprintf(st, "\t%u IRQ\n", r->irq_counter);
}


static error_t
rtl8168_disable(struct device *dev)
{
  rtl8168_t *r = (rtl8168_t *)dev;
  ether_netif_fini(&r->eni);
  return 0;
}

static error_t
rtl8168_shutdown(struct device *dev)
{
  rtl8168_t *r = (rtl8168_t *)dev;
  irq_disable(r->irq);
  netif_detach(&r->eni.eni_ni);
  return 0;
}

static void
rtl8168_dtor(struct device *dev)
{
  free(dev);
}

static const device_class_t rtl8168_device_class = {
  .dc_print_info = rtl8168_print_info,
  .dc_shutdown = rtl8168_shutdown,
  .dc_disable = rtl8168_disable,
  .dc_dtor = rtl8168_dtor,
};


static void
handle_rx(rtl8168_t *r)
{
  while(1) {
    desc_t *rx = r->rx_ring + (r->next_rx & RX_RING_MASK);
    const uint32_t status = rx->cmd_status;
    if(status & DESC_OWN)
      break; // Buffer is owned by NIC

    const int len = (status & 0x3fff) - 4; // Trim CRC
    int discard = 0;

    int flags = PBUF_SOP | PBUF_EOP;

    if(status & RDESC_IS_UDP) {

      if(unlikely(status & RDESC_IP_BAD_CKSUM)) {
        // Can also indicate IPv6, so we just pass it on
      } else if(unlikely(status & RDESC_UDP_BAD_CKSUM)) {
        discard = 1;
        atomic_inc(&r->eni.eni_ni.ni_udp_bad_cksum);
      } else {
        flags |= PBUF_CKSUM_OK;
      }

    } else if (status & RDESC_IS_TCP) {

      if(unlikely(status & RDESC_IP_BAD_CKSUM)) {
        // Can also indicate IPv6, so we just pass it on
      } else if(unlikely(status & RDESC_TCP_BAD_CKSUM)) {
        discard = 1;
        atomic_inc(&r->eni.eni_ni.ni_tcp_bad_cksum);
      } else {
        flags |= PBUF_CKSUM_OK;
      }
    }

    if(len < 0)
      discard = 1;

    if(!discard) {
      void *buf = rx->buf;
      pbuf_t *pb = pbuf_get(0);
      if(pb != NULL) {
        void *nextbuf = pbuf_data_get(0);

        if(nextbuf) {

          pb->pb_data = buf;
          pb->pb_flags = flags;
          pb->pb_offset = 0;

          pb->pb_buflen = len;
          pb->pb_pktlen = len;

          r->eni.eni_stats.rx_byte += len;
          r->eni.eni_stats.rx_pkt++;

          STAILQ_INSERT_TAIL(&r->eni.eni_ni.ni_rx_queue, pb, pb_link);
          netif_wakeup(&r->eni.eni_ni);

          cache_op(nextbuf, PBUF_DATA_SIZE, DCACHE_INVALIDATE);
          rx->buf = nextbuf;
        } else {
          r->eni.eni_stats.rx_sw_qdrop++;
          pbuf_put(pb);
        }
      } else {
        r->eni.eni_stats.rx_sw_qdrop++;
      }
    }
    uint32_t cmd = DESC_OWN | PBUF_DATA_SIZE;
    if((r->next_rx & RX_RING_MASK) == RX_RING_MASK)
      cmd |= DESC_EOR;
    dmb();
    rx->cmd_status = cmd;
    r->next_rx++;
  }
}

static void
handle_tx(rtl8168_t *r)
{
  while(r->tx_rdptr != r->tx_wrptr) {

    int bufidx = r->tx_rdptr & TX_RING_MASK;

    desc_t *tx = r->tx_ring + bufidx;

    uint32_t status = tx->cmd_status;

    if(status & DESC_OWN)
      break;

    pbuf_data_put(r->tx_pbuf_data[bufidx]);
    r->eni.eni_stats.tx_byte += r->tx_size[bufidx];
    r->tx_rdptr++;

    if(status & DESC_FS) {
      r->eni.eni_stats.tx_pkt++;
    }
  }
}


static void
rtl8168_irq(void *arg)
{
  rtl8168_t *r = arg;
  r->irq_counter++;
  uint16_t status = reg_rd16(r->mmio + RTK_IS);
  if(status & RTK_ISR_RX_OK) {
    handle_rx(r);
  }

  if(status & RTK_ISR_TX_OK) {
    handle_tx(r);
  }

  reg_wr16(r->mmio + RTK_IS, status);
}


static error_t
rtl8168_reset(rtl8168_t *r)
{
  uint64_t deadline = clock_get() + 1000;
  reg_wr8(r->mmio + RTK_COMMAND, RTK_CMD_RESET);

  while(1) {
    if((reg_rd8(r->mmio + RTK_COMMAND) & RTK_CMD_RESET) == 0)
      break;
    if(clock_get() > deadline)
      return ERR_TIMEOUT;
  }
  return 0;
}


static error_t
rtl8168_attach(device_t *d)
{
  if(d->d_type != DEVICE_TYPE_PCI)
    return ERR_MISMATCH;

  pci_dev_t *pd = (pci_dev_t *)d;
  if(pd->pd_vid != 0x10ec || pd->pd_pid != 0x8168)
    return ERR_MISMATCH;

  uint64_t mmio = pd->pd_bar[2];
  if(mmio == 0)
    return ERR_INVALID_ADDRESS;

  uint32_t rev = reg_rd(mmio + RTK_TXCFG) & 0x7CC00000;
  if(rev != 0x54000000)
    return ERR_MISMATCH;

  rtl8168_t *r = xalloc(sizeof(rtl8168_t), 0, MEM_CLEAR);
  r->pd = pd;
  r->mmio = mmio;

  for(int i = 0; i < 6; i++) {
    r->eni.eni_addr[i] = reg_rd8(r->mmio + i);
  }

  r->rx_ring = xalloc(RX_RING_SIZE * sizeof(desc_t), 0x100,
                      MEM_TYPE_NO_CACHE | MEM_CLEAR);
  r->tx_ring = xalloc(TX_RING_SIZE * sizeof(desc_t), 0x100,
                      MEM_TYPE_NO_CACHE | MEM_CLEAR);

  for(int i = 0; i < RX_RING_SIZE; i++) {
    desc_t *d = r->rx_ring + i;

    pbuf_t *pb = pbuf_data_get(0);
    cache_op(pb, PBUF_DATA_SIZE, DCACHE_INVALIDATE);
    d->buf = pb;
    d->vlanctl = 0;

    uint32_t cmdstat = PBUF_DATA_SIZE | DESC_OWN;

    if(i == RX_RING_SIZE - 1)
      cmdstat |= DESC_EOR; // End of ring
    d->cmd_status = cmdstat;
  }

  rtl8168_reset(r);

  reg_wr16(r->mmio + RTK_CPLUS_CMD,
           (1 << 7) | // MACSTAT_DIS
           (1 << 5) | // Enable RX checksum offload
           (1 << 3) | // PCI Multi read-write
           (1 << 0) | // TXENB
           0);

  reg_wr16(r->mmio + RTK_IM, 0x5151);

  usleep(10000);

  // init rings

  reg_wr(r->mmio + RTK_RXRING_ADDR_HI, (intptr_t)r->rx_ring >> 32);
  reg_wr(r->mmio + RTK_RXRING_ADDR_LO, (intptr_t)r->rx_ring);
  reg_wr(r->mmio + RTK_TXRING_ADDR_HI, (intptr_t)r->tx_ring >> 32);
  reg_wr(r->mmio + RTK_TXRING_ADDR_LO, (intptr_t)r->tx_ring);

  reg_wr(r->mmio + RTK_MAR0, 0xffffffff); // Accept all multicast
  reg_wr(r->mmio + RTK_MAR4, 0xffffffff); // Accept all multicast

  reg_clr_bit(r->mmio + RTK_MISC, 19); // Clear RXDV_GATED_EN

  reg_wr(r->mmio + RTK_TXCFG,
         0x03000000 | // (Enable ?) Interfame gap
         0x00000500   // Max DMA burst size
         );

  reg_wr8(r->mmio + RTK_EARLY_TX_THRESH, 16);

  uint32_t rxcfg = 0xff00;

  rxcfg |= RL_RXCFG_RX_ALLPHYS;
  rxcfg |= RL_RXCFG_RX_INDIV;
  rxcfg |= RL_RXCFG_RX_BROAD;
  rxcfg |= RL_RXCFG_RX_MULTI;
  reg_wr(r->mmio + RTK_RXCFG, rxcfg);

  reg_wr8(r->mmio + RTK_COMMAND, 0x4 | 0x8); // Enable TX and RX
  reg_wr16(r->mmio + RTK_IMR,
           RTK_ISR_RX_OK |
           RTK_ISR_TX_OK |
           0);

  reg_wr(r->mmio + RTK_MISSEDPKT, 0);
  reg_wr8(r->mmio + RTK_CFG1, reg_rd8(r->mmio + RTK_CFG1) | 0x20);

  r->eni.eni_output = rtl8168_eth_output;

  r->irq = pci_irq_attach_intx(pd, PCI_INTA, IRQ_LEVEL_NET, rtl8168_irq, r);

  snprintf(r->name, sizeof(r->name), "eth_%s", d->d_name);
  r->eni.eni_ni.ni_dev.d_parent = d;
  device_retain(d);
  ether_netif_init(&r->eni, r->name, &rtl8168_device_class);

  evlog(LOG_INFO, "%s: rtl8168 attached IRQ %d", r->name, r->irq);

  net_task_raise(&r->eni.eni_ni.ni_task, NETIF_TASK_STATUS_UP);

  return 0;
}

DRIVER(rtl8168_attach, 1);
