#include <net/ether.h>

#include <mios/pci.h>
#include <mios/mios.h>
#include <mios/driver.h>
#include <mios/eventlog.h>

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <malloc.h>
#include <unistd.h>

#include "reg.h"
#include "irq.h"
#include "cache.h"
#include "barrier.h"

#define RX_RING_SIZE 64
#define TX_RING_SIZE 64
#define RX_RING_MASK (RX_RING_SIZE - 1)
#define TX_RING_MASK (TX_RING_SIZE - 1)

#define TX_DESC_DATA0_DTYPE_MASK_		(0xC0000000)
#define TX_DESC_DATA0_DTYPE_DATA_		(0x00000000)
#define TX_DESC_DATA0_DTYPE_EXT_		(0x40000000)
#define TX_DESC_DATA0_FS_			(0x20000000)
#define TX_DESC_DATA0_LS_			(0x10000000)
#define TX_DESC_DATA0_EXT_			(0x08000000)
#define TX_DESC_DATA0_IOC_			(0x04000000)
#define TX_DESC_DATA0_ICE_			(0x00400000)
#define TX_DESC_DATA0_IPE_			(0x00200000)
#define TX_DESC_DATA0_TPE_			(0x00100000)
#define TX_DESC_DATA0_FCS_			(0x00020000)
#define TX_DESC_DATA0_TSE_			(0x00010000)
#define TX_DESC_DATA0_BUF_LENGTH_MASK_		(0x0000FFFF)
#define TX_DESC_DATA0_EXT_LSO_			(0x00200000)
#define TX_DESC_DATA0_EXT_PAY_LENGTH_MASK_	(0x000FFFFF)
#define TX_DESC_DATA3_FRAME_LENGTH_MSS_MASK_	(0x3FFF0000)

#define MAC_RX_ADDRH			(0x118)
#define MAC_RX_ADDRL			(0x11C)

#define RX_BASE_ADDRH(channel)			(0xC48 + ((channel) << 6))
#define RX_BASE_ADDRL(channel)			(0xC4C + ((channel) << 6))
#define RX_HEAD_WB_ADDRH(channel)	(0xC50 + ((channel) << 6))
#define RX_HEAD_WB_ADDRL(channel)	(0xC54 + ((channel) << 6))
#define RX_HEAD(channel)			(0xC58 + ((channel) << 6))
#define RX_TAIL(channel)			(0xC5C + ((channel) << 6))

#define TX_BASE_ADDRH(channel) (0xD48 + ((channel) << 6))
#define TX_BASE_ADDRL(channel) (0xD4C + ((channel) << 6))
#define TX_HEAD_WB_ADDRH(channel) (0xD50 + ((channel) << 6))
#define TX_HEAD_WB_ADDRL(channel) (0xD54 + ((channel) << 6))
#define TX_HEAD(channel) (0xD58 + ((channel) << 6))
#define TX_TAIL(channel) (0xD5C + ((channel) << 6))

#define BIT(nr)			(1ul << (nr))

#define TX_CFG_A(channel)			(0xD40 + ((channel) << 6))
#define TX_CFG_A_TX_HP_WB_ON_INT_TMR_		BIT(30)
#define TX_CFG_A_TX_TMR_HPWB_SEL_IOC_		(0x10000000)
#define TX_CFG_A_TX_PF_THRES_MASK_		(0x001F0000)
#define TX_CFG_A_TX_PF_THRES_SET_(value)	\
	((((u32)(value)) << 16) & TX_CFG_A_TX_PF_THRES_MASK_)
#define TX_CFG_A_TX_PF_PRI_THRES_MASK_		(0x00001F00)
#define TX_CFG_A_TX_PF_PRI_THRES_SET_(value)	\
	((((u32)(value)) << 8) & TX_CFG_A_TX_PF_PRI_THRES_MASK_)
#define TX_CFG_A_TX_HP_WB_EN_			BIT(5)
#define TX_CFG_A_TX_HP_WB_THRES_MASK_		(0x0000000F)
#define TX_CFG_A_TX_HP_WB_THRES_SET_(value)	\
	(((u32)(value)) & TX_CFG_A_TX_HP_WB_THRES_MASK_)


#define TX_CFG_B(channel)			(0xD44 + ((channel) << 6))
#define TX_CFG_B_TDMABL_512_			(0x00040000)
#define TX_CFG_B_TX_RING_LEN_MASK_		(0x0000FFFF)


/* OWN bit is set. ie, Descs are owned by RX DMAC */
#define RX_DESC_DATA0_OWN_                (0x00008000)
/* OWN bit is clear. ie, Descs are owned by host */
#define RX_DESC_DATA0_FS_                 (0x80000000)
#define RX_DESC_DATA0_LS_                 (0x40000000)
#define RX_DESC_DATA0_FRAME_LENGTH_MASK_  (0x3FFF0000)
#define RX_DESC_DATA0_FRAME_LENGTH_GET_(data0)	\
	(((data0) & RX_DESC_DATA0_FRAME_LENGTH_MASK_) >> 16)
#define RX_DESC_DATA0_EXT_                (0x00004000)
#define RX_DESC_DATA0_BUF_LENGTH_MASK_    (0x00003FFF)
#define RX_DESC_DATA1_STATUS_ICE_         (0x00020000)
#define RX_DESC_DATA1_STATUS_TCE_         (0x00010000)
#define RX_DESC_DATA1_STATUS_ICSM_        (0x00000001)
#define RX_DESC_DATA2_TS_NS_MASK_         (0x3FFFFFFF)

#define ID_REV				(0x00)
#define ID_REV_ID_MASK_			(0xFFFF0000)
#define ID_REV_ID_LAN7430_		(0x74300000)
#define ID_REV_ID_LAN7431_		(0x74310000)
#define ID_REV_ID_LAN743X_		(0x74300000)

#define HW_CFG					(0x010)
#define HW_CFG_RST_PROTECT_PCIE_		BIT(19)
#define HW_CFG_HOT_RESET_DIS_			BIT(15)
#define HW_CFG_D3_VAUX_OVR_			BIT(14)
#define HW_CFG_D3_RESET_DIS_			BIT(13)
#define HW_CFG_RST_PROTECT_			BIT(12)
#define HW_CFG_RELOAD_TYPE_ALL_			(0x00000FC0)
#define HW_CFG_EE_OTP_RELOAD_			BIT(4)
#define HW_CFG_LRST_				BIT(1)

#define RX_CFG_A(channel)			(0xC40 + ((channel) << 6))
#define RX_CFG_A_RX_HP_WB_EN_ (0x00000020)

#define RX_CFG_B(channel)			(0xC44 + ((channel) << 6))
#define RX_CFG_B_TS_ALL_RX_			BIT(29)
#define RX_CFG_B_RX_PAD_MASK_			(0x03000000)
#define RX_CFG_B_RX_PAD_0_			(0x00000000)
#define RX_CFG_B_RX_PAD_2_			(0x02000000)
#define RX_CFG_B_RDMABL_512_			(0x00040000)
#define RX_CFG_B_RX_RING_LEN_MASK_		(0x0000FFFF)

#define MAC_MII_ACC			(0x120)
#define MAC_MII_ACC_MDC_CYCLE_SHIFT_	(16)
#define MAC_MII_ACC_MDC_CYCLE_MASK_	(0x00070000)
#define MAC_MII_ACC_MDC_CYCLE_2_5MHZ_	(0)
#define MAC_MII_ACC_MDC_CYCLE_5MHZ_	(1)
#define MAC_MII_ACC_MDC_CYCLE_12_5MHZ_	(2)
#define MAC_MII_ACC_MDC_CYCLE_25MHZ_	(3)
#define MAC_MII_ACC_MDC_CYCLE_1_25MHZ_	(4)
#define MAC_MII_ACC_PHY_ADDR_SHIFT_	(11)
#define MAC_MII_ACC_PHY_ADDR_MASK_	(0x0000F800)
#define MAC_MII_ACC_MIIRINDA_SHIFT_	(6)
#define MAC_MII_ACC_MIIRINDA_MASK_	(0x000007C0)
#define MAC_MII_ACC_MII_READ_		(0x00000000)
#define MAC_MII_ACC_MII_WRITE_		(0x00000002)
#define MAC_MII_ACC_MII_BUSY_		BIT(0)

#define MAC_MII_DATA			(0x124)

#define INT_EN_SET			(0x788)
#define INT_EN_CLR			(0x78C)

#define INT_STS        (0x780)

#define INT_BIT_DMA_RX_(channel)	BIT(24 + (channel))
#define INT_BIT_ALL_RX_			(0x0F000000)
#define INT_BIT_DMA_TX_(channel)	BIT(16 + (channel))
#define INT_BIT_ALL_TX_			(0x000F0000)
#define INT_BIT_SW_GP_			BIT(9)
#define INT_BIT_1588_			BIT(7)
#define INT_BIT_ALL_OTHER_		(INT_BIT_SW_GP_ | INT_BIT_1588_)
#define INT_BIT_MAS_			BIT(0)

#define DMAC_INT_STS				(0xC10)
#define DMAC_INT_EN_SET				(0xC14)
#define DMAC_INT_EN_CLR				(0xC18)
#define DMAC_INT_BIT_RXFRM_(channel)		BIT(16 + (channel))
#define DMAC_INT_BIT_TX_IOC_(channel)		BIT(0 + (channel))


#define FCT_RX_CTL			(0xAC)
#define FCT_RX_CTL_EN_(channel)		BIT(28 + (channel))
#define FCT_RX_CTL_DIS_(channel)	BIT(24 + (channel))
#define FCT_RX_CTL_RESET_(channel)	BIT(20 + (channel))

#define FCT_TX_CTL			(0xC4)
#define FCT_TX_CTL_EN_(channel)		BIT(28 + (channel))
#define FCT_TX_CTL_DIS_(channel)	BIT(24 + (channel))
#define FCT_TX_CTL_RESET_(channel)	BIT(20 + (channel))


#define DMAC_CMD				(0xC0C)
#define DMAC_CMD_TX_SWR_(channel)		BIT(24 + (channel))
#define DMAC_CMD_START_T_(channel)		BIT(20 + (channel))
#define DMAC_CMD_STOP_T_(channel)		BIT(16 + (channel))
#define DMAC_CMD_RX_SWR_(channel)		BIT(8 + (channel))
#define DMAC_CMD_START_R_(channel)		BIT(4 + (channel))
#define DMAC_CMD_STOP_R_(channel)		BIT(0 + (channel))

#define MAC_CR (0x100)
#define MAC_CR_ADD_ (0x00001000)
#define MAC_CR_ASD_ (0x00000800)

typedef struct desc {
	uint32_t     cmd_status;
  union {
	  void         *buf;
    struct {
      uint32_t     data1;
      uint32_t     data2;
    };
  };
	uint32_t     data3;
} __attribute__((packed, aligned(16))) desc_t;


typedef struct lan743x {
  ether_netif_t eni;
  pci_dev_t *pd;
  long mmio;

  desc_t *rx_ring;
  desc_t *tx_ring;

  thread_t *phy_thread;
  int phy_run;

  void *rx_head_ptr;
  uint32_t rx_last_head;

  int next_rx;

  int tx_wrptr;
  int tx_rdptr;
  int *tx_wr_back_ptr;

  pbuf_t *tx_pbuf_data[TX_RING_SIZE];
  pbuf_t *rx_pbuf_data[TX_RING_SIZE];  
  uint16_t tx_size[TX_RING_SIZE];

  uint32_t irq_counter;
  uint32_t irq;
  char name[16];

} lan743x_t;


static error_t
lan743x_eth_output(struct ether_netif *eni, pbuf_t *pkt, int flags)
{
  lan743x_t *r = (lan743x_t *)eni;

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

  for(pb = pkt; pb != NULL; pb = pb->pb_next) {

    int bufidx = wrptr & TX_RING_MASK;

    desc_t *tx = r->tx_ring + bufidx;

    cache_op(pb->pb_data, pb->pb_buflen, DCACHE_CLEAN | DCACHE_INVALIDATE);

    r->tx_pbuf_data[bufidx] = pb->pb_data;
    r->tx_size[bufidx] = pb->pb_buflen;

    tx->buf = pb->pb_data + pb->pb_offset;

    uint32_t cmd = pb->pb_buflen |
      TX_DESC_DATA0_DTYPE_DATA_ |
		  TX_DESC_DATA0_FCS_;


    if(pb->pb_flags & PBUF_SOP) {
      cmd |= TX_DESC_DATA0_FS_;
    }
    if(pb->pb_flags & PBUF_EOP) {
      cmd |= TX_DESC_DATA0_IOC_ | TX_DESC_DATA0_LS_;
    }
    tx->cmd_status = cmd;
    wrptr++;
  }

  // Wait until the end to hand over the descriptor for the first fragment
  dmb();
  reg_wr(r->mmio + TX_TAIL(0), wrptr & TX_RING_MASK);
  dmb();

  r->tx_wrptr = wrptr;

  for(; pkt != NULL; pkt = pb) {
    pb = STAILQ_NEXT(pkt, pb_link);
    pbuf_put(pkt); // Free header, data will be free'd after TX is done
  }

  irq_permit(q);
  return 0;
}

static void
handle_rx(lan743x_t *r)
{
  while(1) {
    desc_t *rx = r->rx_ring + (r->next_rx & RX_RING_MASK);
    const uint32_t status = rx->cmd_status;
    if(status & RX_DESC_DATA0_OWN_)
      break; // Buffer is owned by NIC

    const int len = RX_DESC_DATA0_FRAME_LENGTH_GET_(status);
    int discard = 0;

    int flags = PBUF_SOP | PBUF_EOP;
    const uint32_t data1 = rx->data1;
    if(unlikely(data1 & RX_DESC_DATA1_STATUS_ICE_)) {
      discard = 1;
      atomic_inc(&r->eni.eni_ni.ni_ipv4_bad_cksum);
    } else if(unlikely(status & RX_DESC_DATA1_STATUS_TCE_)) {
      discard = 1;
      atomic_inc(&r->eni.eni_ni.ni_tcp_bad_cksum);
    } else {
      flags |= PBUF_CKSUM_OK;
    }

    if(len < 0)
      discard = 1;

    if(!discard) {
      void *buf = r->rx_pbuf_data[r->next_rx & RX_RING_MASK];
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
          r->rx_pbuf_data[r->next_rx & RX_RING_MASK] = nextbuf;
        } else {
          r->eni.eni_stats.rx_sw_qdrop++;
          pbuf_put(pb);
        }
      } else {
        r->eni.eni_stats.rx_sw_qdrop++;
      }
    }
    uint32_t cmd = RX_DESC_DATA0_OWN_ | PBUF_DATA_SIZE;
    dmb();
    rx->cmd_status = cmd;
    r->next_rx++;
  }
}


static void
handle_tx(lan743x_t *r)
{
  while(r->tx_rdptr != *r->tx_wr_back_ptr) {

    int bufidx = r->tx_rdptr & TX_RING_MASK;

    desc_t *tx = r->tx_ring + bufidx;

    memset(tx, 0, sizeof(*tx));

    pbuf_data_put(r->tx_pbuf_data[bufidx]);
    r->eni.eni_stats.tx_byte += r->tx_size[bufidx];
    r->eni.eni_stats.tx_pkt++;
    r->tx_rdptr++;
  }
}


static void
lan743x_irq(void *arg)
{
  lan743x_t *r = arg;
  r->irq_counter++;
  uint32_t status = reg_rd(r->mmio + INT_STS) & reg_rd(r->mmio + INT_EN_SET);

  if (!(status & INT_BIT_MAS_))
    /* master bit not set, can't be ours */
    return;
  // disable interrupts
  reg_wr(r->mmio + INT_EN_CLR, status);
  // Clear handled bits
  reg_wr(r->mmio + INT_STS, status); 

  if(status & INT_BIT_DMA_RX_(0)) {
    handle_rx(r);
  }

  if(status & INT_BIT_DMA_TX_(0)) {
    handle_tx(r);
  }

  if(status & INT_BIT_ALL_OTHER_) {
    //handle_link_change(r);
  }
  // enable interrupts
  reg_wr16(r->mmio + INT_EN_SET, status);
}


static error_t
lan743x_reset(lan743x_t *r)
{
  uint64_t deadline = clock_get() + 100000;
  uint32_t data = reg_rd(r->mmio + HW_CFG);
  data |= HW_CFG_LRST_;
  reg_wr(r->mmio + HW_CFG, data);

  while(1) {
    if((reg_rd(r->mmio + HW_CFG) & HW_CFG_LRST_) == 0)
      break;
    if(clock_get() > deadline)
      return ERR_TIMEOUT;
  }
  return 0;
}

static void
mii_write(void *arg, uint16_t reg, uint16_t value)
{
  lan743x_t *r = arg;

  reg_wr(r->mmio + MAC_MII_DATA, value);
	uint32_t acc =  ((reg << MAC_MII_ACC_MIIRINDA_SHIFT_) &
		MAC_MII_ACC_MIIRINDA_MASK_ )
		 | MAC_MII_ACC_MII_WRITE_
  	 | MAC_MII_ACC_MII_BUSY_;

  reg_wr(r->mmio +  MAC_MII_ACC, acc);
  while(reg_rd(r->mmio + MAC_MII_ACC) & 1) {
  }

}


static uint16_t
mii_read(void *arg, uint16_t reg)
{
  lan743x_t *r = arg;

	uint32_t acc =  ((reg << MAC_MII_ACC_MIIRINDA_SHIFT_) &
		MAC_MII_ACC_MIIRINDA_MASK_ )
		 | MAC_MII_ACC_MII_READ_
  	 | MAC_MII_ACC_MII_BUSY_;
  reg_wr(r->mmio +  MAC_MII_ACC, acc);
  while(reg_rd(r->mmio + MAC_MII_ACC) & 1) {
  }
  return reg_rd(r->mmio + MAC_MII_DATA);  
}

static void
lan7431_print_info(struct device *dev, struct stream *st)
{
  lan743x_t *r = (lan743x_t *)dev;
  ether_print((ether_netif_t *)dev, st);
  stprintf(st, "\t%u IRQ\n", r->irq_counter);
}


static error_t
lan743x_disable(struct device *dev)
{
  lan743x_t *r = (lan743x_t *)dev;
  irq_disable(r->irq);
  ether_netif_fini(&r->eni);
  return 0;
}

static error_t
lan743x_shutdown(struct device *dev)
{
  lan743x_t *r = (lan743x_t *)dev;

  netif_detach(&r->eni.eni_ni);
  r->phy_run = 0;
  thread_join(r->phy_thread);  
  return 0;
}

static void
lan743x_dtor(struct device *dev)
{
  free(dev);
}

static const device_class_t lan743x_device_class = {
  .dc_print_info = lan7431_print_info,
  .dc_disable = lan743x_disable,
  .dc_shutdown = lan743x_shutdown,
  .dc_dtor = lan743x_dtor,
};

static void *__attribute__((noreturn))
lan743x_phy_thread(void *arg)
{
  lan743x_t *r= arg;
  int current_up = 0;

  while(r->phy_run) {
    mii_read(r, 1);
    int n = mii_read(r, 1);
    int up = !!(n & 4);

    if(!current_up && up) {

      // FIXME: Configure correct speed and duplex in MAC

      current_up = 1;
      net_task_raise(&r->eni.eni_ni.ni_task, NETIF_TASK_STATUS_UP);
    } else if(current_up && !up) {
      current_up = 0;
      net_task_raise(&r->eni.eni_ni.ni_task, NETIF_TASK_STATUS_DOWN);
    }
    usleep(100000);
  }
  thread_exit(NULL);
}

static error_t
lan743x_attach(device_t *d)
{
  if(d->d_type != DEVICE_TYPE_PCI)
    return ERR_MISMATCH;

  pci_dev_t *pd = (pci_dev_t *)d;
  if(pd->pd_vid != 0x1055 || pd->pd_pid != 0x7431)
    return ERR_MISMATCH;

  uint64_t mmio = pd->pd_bar[0];
  if(mmio == 0)
    return ERR_INVALID_ADDRESS;

  uint32_t rev = reg_rd(mmio + ID_REV) & ID_REV_ID_MASK_;
  if(rev != ID_REV_ID_LAN7431_)
    return ERR_MISMATCH;

  lan743x_t *r = xalloc(sizeof(lan743x_t), 0, MEM_CLEAR);
  r->pd = pd;
  r->mmio = mmio;

  uint32_t mac_addr_lo = reg_rd(mmio + MAC_RX_ADDRL);
  uint32_t mac_addr_hi = reg_rd(mmio + MAC_RX_ADDRH);
	r->eni.eni_addr[0] = mac_addr_lo & 0xFF;
	r->eni.eni_addr[1] = (mac_addr_lo >> 8) & 0xFF;
	r->eni.eni_addr[2] = (mac_addr_lo >> 16) & 0xFF;
	r->eni.eni_addr[3] = (mac_addr_lo >> 24) & 0xFF;
	r->eni.eni_addr[4] = mac_addr_hi & 0xFF;
	r->eni.eni_addr[5] = (mac_addr_hi >> 8) & 0xFF;

  r->rx_ring = xalloc(RX_RING_SIZE * sizeof(desc_t), 0x100,
                      MEM_TYPE_NO_CACHE | MEM_CLEAR);
  r->rx_head_ptr= xalloc(sizeof(r->rx_head_ptr), 0x100,
                      MEM_TYPE_NO_CACHE | MEM_CLEAR);

  r->tx_ring = xalloc(TX_RING_SIZE * sizeof(desc_t), 0x100,
                      MEM_TYPE_NO_CACHE | MEM_CLEAR);
  r->tx_wr_back_ptr= xalloc(sizeof(r->tx_wr_back_ptr), 0x100,
                      MEM_TYPE_NO_CACHE | MEM_CLEAR);

  for(int i = 0; i < RX_RING_SIZE; i++) {
    desc_t *d = r->rx_ring + i;

    pbuf_t *pb = pbuf_data_get(0);
    cache_op(pb, PBUF_DATA_SIZE, DCACHE_INVALIDATE);
    d->buf = pb;
    r->rx_pbuf_data[i] = pb;
    d->cmd_status = RX_DESC_DATA0_OWN_ | PBUF_DATA_SIZE;
    d->data3 = 0;
    if(i == RX_RING_SIZE - 1)
      reg_wr(mmio + RX_TAIL(0), i); 
  }

  lan743x_reset(r);

  usleep(10000);

  // init rings

  reg_wr(r->mmio + RX_BASE_ADDRH(0), (intptr_t)r->rx_ring >> 32);
  reg_wr(r->mmio + RX_BASE_ADDRL(0), (intptr_t)r->rx_ring);

  reg_wr(r->mmio + RX_HEAD_WB_ADDRH(0), (intptr_t)r->rx_head_ptr >> 32);
  reg_wr(r->mmio + RX_HEAD_WB_ADDRL(0), (intptr_t)r->rx_head_ptr);

  reg_wr(r->mmio + RX_CFG_A(0), RX_CFG_A_RX_HP_WB_EN_);
  uint32_t temp = reg_rd(r->mmio + RX_CFG_B(0));
  temp &= ~RX_CFG_B_RX_PAD_MASK_;
  temp |= RX_CFG_B_RX_PAD_0_;
  /* NOTE: use RX_CFG_B_RX_PAD_2_ instead if you want each packet to have
  * a 2 byte padding at the beginning of the buffer. This allows the
  * IP header to have better alignment.
  */
  temp &= ~RX_CFG_B_RX_RING_LEN_MASK_;
  temp |= (RX_RING_SIZE & RX_CFG_B_RX_RING_LEN_MASK_);
  reg_wr(r->mmio + RX_CFG_B(0), temp);


 /* set head write back address */
 /* set tail and head so all descriptors belong to the LAN743x */
  r->rx_last_head = reg_rd(r->mmio + RX_HEAD(0));
  reg_wr(r->mmio + DMAC_CMD, DMAC_CMD_START_R_(0));

  // fct rx rst
  
  reg_wr(r->mmio + FCT_RX_CTL, FCT_RX_CTL_RESET_(0));
  while (!(reg_rd(r->mmio + FCT_RX_CTL) & FCT_RX_CTL_RESET_(0))){
  }

  reg_wr(r->mmio + FCT_RX_CTL, FCT_RX_CTL_EN_(0));

  /* setup tx */
  reg_wr(r->mmio + FCT_TX_CTL, FCT_TX_CTL_RESET_(0));
  while (!(reg_rd(r->mmio + FCT_RX_CTL) & FCT_RX_CTL_RESET_(0))){
  }
  reg_wr(r->mmio + FCT_TX_CTL, FCT_TX_CTL_EN_(0));
  reg_wr(r->mmio + DMAC_CMD, DMAC_CMD_TX_SWR_(0));

  reg_wr(r->mmio + TX_BASE_ADDRH(0), (intptr_t)r->tx_ring >> 32);
  reg_wr(r->mmio + TX_BASE_ADDRL(0), (intptr_t)r->tx_ring);

  reg_wr(r->mmio + TX_HEAD_WB_ADDRH(0), (intptr_t)r->tx_wr_back_ptr >> 32);
  reg_wr(r->mmio + TX_HEAD_WB_ADDRL(0), (intptr_t)r->tx_wr_back_ptr);
 
   /* set ring size */

  uint32_t cfg_b = reg_rd(r->mmio + TX_CFG_B(0));
  cfg_b &= ~TX_CFG_B_TX_RING_LEN_MASK_;
  cfg_b |= (TX_RING_SIZE & TX_CFG_B_TX_RING_LEN_MASK_);
  reg_wr(r->mmio + TX_CFG_B(0), cfg_b);
  /* Enable interrupt on completion and head pointer writeback */
  uint32_t cfg_a = TX_CFG_A_TX_TMR_HPWB_SEL_IOC_ | TX_CFG_A_TX_HP_WB_EN_;
  reg_wr(r->mmio + TX_CFG_A(0), cfg_a);
  /* set head pointer write back address */
  /* due to previous reset, tx_last_head should be 0 after this READ */
  r->tx_wrptr = reg_rd(r->mmio + TX_HEAD(0));
  /* set tail to 0 */
  r->tx_rdptr = 0;
  reg_wr(r->mmio + TX_TAIL(0), r->tx_rdptr);

  /* enable interrupts */

  reg_wr(r->mmio + INT_EN_SET, INT_BIT_DMA_RX_(0) | INT_BIT_DMA_TX_(0) | INT_BIT_MAS_);
  reg_wr(r->mmio + DMAC_INT_EN_SET, DMAC_INT_BIT_RXFRM_(0) | DMAC_INT_BIT_TX_IOC_(0));
 
  reg_wr(r->mmio + DMAC_CMD, DMAC_CMD_START_T_(0));
  reg_wr(r->mmio + DMAC_CMD, DMAC_CMD_START_R_(0));
  // Start RX and TX

  while((reg_rd(r->mmio + DMAC_CMD) & (DMAC_CMD_START_T_(0)| DMAC_CMD_STOP_T_(0))) 
          ==(DMAC_CMD_START_T_(0) | DMAC_CMD_STOP_T_(0 ))) {
  }
  reg_wr(r->mmio + DMAC_CMD, DMAC_CMD_START_T_(0));


  r->eni.eni_output = lan743x_eth_output;

  r->irq = pci_irq_attach_intx(pd, PCI_INTA, IRQ_LEVEL_NET, lan743x_irq, r);

  snprintf(r->name, sizeof(r->name), "eth_%s", d->d_name);
  r->eni.eni_ni.ni_dev.d_parent = d;
  device_retain(d);
  ether_netif_init(&r->eni, r->name, &lan743x_device_class);

  evlog(LOG_INFO, "%s: lan743x attached IRQ %d", r->name, r->irq);

  uint16_t anar = mii_read(r, 0x4);
  uint16_t gtcr = mii_read(r, 0x9);

  // If we're not announcing all full duplex rates over autoneg, make sure we do
  if(anar != 0x1e1 || gtcr != 0x200) {
    mii_write(r, 0x4, 0x01e1);
    mii_write(r, 0x9, 0x0200);

    // Reset and restart autoneg
    mii_write(r, 0, 0x9200);
  }

  uint32_t mac_cr = reg_rd(r->mmio + MAC_CR);
  reg_wr(r->mmio + MAC_CR, mac_cr | MAC_CR_ADD_ | MAC_CR_ASD_);
  r->phy_run = 1;
  r->phy_thread = thread_create(lan743x_phy_thread, r, 0, "phy",
                TASK_NO_FPU | TASK_NO_DMA_STACK, 4);

  return 0;
}

DRIVER(lan743x_attach, 1);

