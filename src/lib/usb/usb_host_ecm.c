// CDC-ECM (Ethernet) class driver for the USB host stack. Each attached
// device becomes an Ethernet interface (ecm0, ecm1, ...)
//
// USB transfers complete at IRQ_LEVEL_IO, which preempts code that has
// IRQ_LEVEL_NET blocked, so the completion callbacks don't touch pbufs.
// They hand over to a net task that runs on the network thread.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <malloc.h>
#include <sys/param.h>

#include <mios/task.h>
#include <mios/error.h>

#include <net/pbuf.h>
#include <net/ether.h>

#include <usb/usb_host.h>

#include "irq.h"

#define ECM_BUF_SIZE    1536  // Max frame (1514), multiple of the HS bulk MPS
#define ECM_RX_BUFS     2
#define ECM_TXQ_LEN     16

#define CDC_SET_ETHERNET_PACKET_FILTER 0x43

#define CDC_PACKET_TYPE_ALL_MULTICAST  0x02
#define CDC_PACKET_TYPE_DIRECTED       0x04
#define CDC_PACKET_TYPE_BROADCAST      0x08

#define CDC_SUBTYPE_UNION    0x06
#define CDC_SUBTYPE_ETHERNET 0x0f

#define TASK_RX  0x1
#define TASK_TX  0x2

typedef struct usb_host_ecm {
  ether_netif_t eni;

  usb_host_device_t *ud;
  usb_host_pipe_t *in;
  usb_host_pipe_t *out;
  uint16_t out_mps;

  net_task_t task;

  // Protected by IRQ_LEVEL_IO. An RX buffer is either submitted, full
  // (waiting for the net task) or free. Only one is submitted at a time
  usb_host_xfer_t rx_xfer[ECM_RX_BUFS];
  uint8_t *rx_dma[ECM_RX_BUFS];
  uint8_t rx_full[ECM_RX_BUFS];
  uint8_t rx_running;
  uint8_t rx_next;        // Buffer to submit next
  uint8_t tx_running;
  uint8_t gone;

  usb_host_xfer_t tx_xfer;
  uint8_t *tx_dma;

  // Frames waiting for the TX transfer. Protected by IRQ_LEVEL_NET
  pbuf_t *txq[ECM_TXQ_LEN];
  uint8_t txq_rdptr;      // Free running
  uint8_t txq_wrptr;

  uint32_t rx_errors;
  uint32_t tx_errors;

  char name[8];
} usb_host_ecm_t;


static int ecm_unit;


// Called with IRQ_LEVEL_IO blocked
static void
ecm_rx_start(usb_host_ecm_t *e)
{
  if(e->rx_running || e->gone)
    return;
  const int i = e->rx_next;
  if(e->rx_full[i])
    return; // The net task restarts us once it has emptied it

  e->rx_xfer[i].ux_buf = e->rx_dma[i];
  e->rx_xfer[i].ux_len = ECM_BUF_SIZE;
  if(!usb_host_submit(e->ud, e->in, &e->rx_xfer[i])) {
    e->rx_running = 1;
    e->rx_next = (i + 1) % ECM_RX_BUFS;
  }
}


static void
ecm_rx_done(usb_host_xfer_t *ux)
{
  usb_host_ecm_t *e = ux->ux_opaque;
  e->rx_running = 0;
  if(ux->ux_result < 0) {
    e->rx_errors++;
    return; // Gone, or will be shortly
  }
  if(ux->ux_result > 0)
    e->rx_full[ux - e->rx_xfer] = 1;
  ecm_rx_start(e);
  net_task_raise(&e->task, TASK_RX);
}


static void
ecm_tx_done(usb_host_xfer_t *ux)
{
  usb_host_ecm_t *e = ux->ux_opaque;
  e->tx_running = 0;
  if(ux->ux_result < 0)
    e->tx_errors++;
  net_task_raise(&e->task, TASK_TX);
}


// Copy a frame into pbufs and queue it on the netif. IRQ_LEVEL_NET blocked
static void
ecm_rx_frame(usb_host_ecm_t *e, const uint8_t *frame, size_t len)
{
  ether_netif_t *eni = &e->eni;
  struct pbuf_queue pbq;
  STAILQ_INIT(&pbq);

  // Leave 2 bytes so the IP header after the 14 byte Ethernet header
  // ends up 4-byte aligned
  size_t offset = 2;
  size_t pos = 0;
  int first = 1;

  while(pos < len) {
    pbuf_t *pb = pbuf_get(0);
    void *buf = pb ? pbuf_data_get(0) : NULL;
    if(buf == NULL) {
      if(pb)
        pbuf_put(pb);
      pbuf_free_queue_irq_blocked(&pbq);
      eni->eni_stats.rx_sw_qdrop++;
      return;
    }
    const size_t chunk = MIN(len - pos, PBUF_DATA_SIZE - offset);
    memcpy(buf + offset, frame + pos, chunk);
    pb->pb_data = buf;
    pb->pb_offset = offset;
    pb->pb_buflen = chunk;
    pb->pb_flags = first ? PBUF_SOP : 0;
    pb->pb_pktlen = len;
    STAILQ_INSERT_TAIL(&pbq, pb, pb_link);
    pos += chunk;
    offset = 0;
    first = 0;
  }
  STAILQ_LAST(&pbq, pbuf, pb_link)->pb_flags |= PBUF_EOP;

  eni->eni_stats.rx_pkt++;
  eni->eni_stats.rx_byte += len;
  STAILQ_CONCAT(&eni->eni_ni.ni_rx_queue, &pbq);
}


// Copy the frame into the DMA buffer and send it. Frees the pbuf.
// IRQ_LEVEL_NET blocked, the TX transfer is idle
static void
ecm_tx_start(usb_host_ecm_t *e, pbuf_t *pkt)
{
  size_t len = 0;
  for(pbuf_t *pb = pkt; pb != NULL; pb = pb->pb_next) {
    if(len + pb->pb_buflen > ECM_BUF_SIZE - 1) {
      len = 0;
      break;
    }
    memcpy(e->tx_dma + len, pbuf_data(pb, 0), pb->pb_buflen);
    len += pb->pb_buflen;
  }
  const size_t pktlen = len;
  pbuf_free_irq_blocked(pkt);

  if(len == 0) {
    e->eni.eni_stats.tx_qdrop++;
    return;
  }

  // A transfer that is a multiple of the max packet size would need a
  // zero length packet to end it. Pad with a byte instead, like Linux
  if(len % e->out_mps == 0)
    e->tx_dma[len++] = 0;

  e->tx_xfer.ux_buf = e->tx_dma;
  e->tx_xfer.ux_len = len;

  int q = irq_forbid(IRQ_LEVEL_IO);
  error_t err = e->gone ? ERR_NOT_CONNECTED :
    usb_host_submit(e->ud, e->out, &e->tx_xfer);
  if(!err)
    e->tx_running = 1;
  irq_permit(q);

  if(err) {
    e->eni.eni_stats.tx_qdrop++;
  } else {
    e->eni.eni_stats.tx_pkt++;
    e->eni.eni_stats.tx_byte += pktlen;
  }
}


static error_t
ecm_output(struct ether_netif *eni, pbuf_t *pkt, pbuf_tx_cb_t *txcb,
           uint32_t id)
{
  usb_host_ecm_t *e = (usb_host_ecm_t *)eni;

  int q = irq_forbid(IRQ_LEVEL_NET);

  const uint8_t queued = e->txq_wrptr - e->txq_rdptr;
  if(e->gone || queued == ECM_TXQ_LEN) {
    eni->eni_stats.tx_qdrop++;
    pbuf_free_irq_blocked(pkt);
    irq_permit(q);
    return ERR_QUEUE_FULL;
  }

  if(queued == 0 && !e->tx_running)
    ecm_tx_start(e, pkt);
  else
    e->txq[e->txq_wrptr++ % ECM_TXQ_LEN] = pkt;
  irq_permit(q);
  return 0;
}


static void
ecm_task_cb(net_task_t *nt, uint32_t signals)
{
  usb_host_ecm_t *e = (void *)nt - offsetof(usb_host_ecm_t, task);

  int q = irq_forbid(IRQ_LEVEL_NET);

  if(signals & TASK_RX) {
    int wakeup = 0;
    for(int n = 0; n < ECM_RX_BUFS; n++) {
      // Oldest full buffer first: the one after the submitted one
      const int i = (e->rx_next + n) % ECM_RX_BUFS;
      if(!e->rx_full[i])
        continue;
      ecm_rx_frame(e, e->rx_dma[i], e->rx_xfer[i].ux_result);
      wakeup = 1;
      int q2 = irq_forbid(IRQ_LEVEL_IO);
      e->rx_full[i] = 0;
      ecm_rx_start(e);
      irq_permit(q2);
    }
    if(wakeup)
      netif_wakeup(&e->eni.eni_ni);
  }

  if(signals & TASK_TX && !e->tx_running && e->txq_rdptr != e->txq_wrptr)
    ecm_tx_start(e, e->txq[e->txq_rdptr++ % ECM_TXQ_LEN]);
  irq_permit(q);
}


static void
ecm_print_info(struct device *dev, struct stream *st)
{
  usb_host_ecm_t *e = (usb_host_ecm_t *)dev;
  ether_print(&e->eni, st);
  stprintf(st, "USB: %s  RX errors: %u  TX errors: %u  TX queue: %d\n",
           e->gone ? "detached" : e->ud->ud_dev.d_name,
           e->rx_errors, e->tx_errors,
           (uint8_t)(e->txq_wrptr - e->txq_rdptr));
}


// Called on the network thread when the netif is detached
static error_t
ecm_disable(struct device *dev)
{
  usb_host_ecm_t *e = (usb_host_ecm_t *)dev;
  ether_netif_fini(&e->eni);
  net_task_cancel(&e->task);
  while(e->txq_rdptr != e->txq_wrptr)
    pbuf_free(e->txq[e->txq_rdptr++ % ECM_TXQ_LEN]);
  return 0;
}


static void
ecm_dtor(struct device *dev)
{
  usb_host_ecm_t *e = (usb_host_ecm_t *)dev;
  for(int i = 0; i < ECM_RX_BUFS; i++)
    free(e->rx_dma[i]);
  free(e->tx_dma);
  free(e);
}


static const ethmac_device_class_t ecm_device_class = {
  .dc = {
    .dc_class_name = "usb-ecm",
    .dc_print_info = ecm_print_info,
    .dc_disable = ecm_disable,
    .dc_dtor = ecm_dtor,
  }
};


static int
hexdigit(char c)
{
  if(c >= '0' && c <= '9')
    return c - '0';
  if(c >= 'a' && c <= 'f')
    return c - 'a' + 10;
  if(c >= 'A' && c <= 'F')
    return c - 'A' + 10;
  return -1;
}


// iMACAddress is a string of 12 hex digits
static error_t
parse_mac(const char *s, uint8_t mac[6])
{
  if(s == NULL || strlen(s) != 12)
    return ERR_MALFORMED;
  for(int i = 0; i < 6; i++) {
    const int hi = hexdigit(s[i * 2]);
    const int lo = hexdigit(s[i * 2 + 1]);
    if(hi < 0 || lo < 0)
      return ERR_MALFORMED;
    mac[i] = (hi << 4) | lo;
  }
  return 0;
}


static void *
ecm_probe(usb_host_device_t *ud, const struct usb_interface_descriptor *id)
{
  if(id->bInterfaceClass != 0x02 || id->bInterfaceSubClass != 0x06)
    return NULL;

  int data_ifnum = -1;
  int mac_index = 0;
  const uint8_t *cs = (const void *)id;
  while((cs = usb_host_find_desc(ud, cs, USB_DESC_TYPE_CS_INTERFACE))) {
    if(cs[0] >= 5 && cs[2] == CDC_SUBTYPE_UNION)
      data_ifnum = cs[4];
    if(cs[0] >= 13 && cs[2] == CDC_SUBTYPE_ETHERNET)
      mac_index = cs[3];
  }
  if(data_ifnum < 0 || data_ifnum >= 32)
    return NULL;

  // The data interface has no endpoints in alternate setting 0, which
  // keeps the function idle. Alternate setting 1 has the bulk pair
  const struct usb_interface_descriptor *did =
    usb_host_find_interface_alt(ud, data_ifnum, 1);
  if(did == NULL || did->bInterfaceClass != 0x0a)
    return NULL;

  const struct usb_endpoint_descriptor *in_ed =
    usb_host_find_endpoint(ud, did, USB_ENDPOINT_BULK, 0x80);
  const struct usb_endpoint_descriptor *out_ed =
    usb_host_find_endpoint(ud, did, USB_ENDPOINT_BULK, 0);
  if(in_ed == NULL || out_ed == NULL)
    return NULL;

  usb_host_ecm_t *e = calloc(1, sizeof(usb_host_ecm_t));

  char *macstr = usb_host_get_string(ud, mac_index);
  if(parse_mac(macstr, e->eni.eni_addr)) {
    printf("%s: CDC-ECM: Bad MAC address string\n", ud->ud_dev.d_name);
    free(macstr);
    free(e);
    return NULL;
  }
  free(macstr);

  if(usb_host_set_interface(ud, data_ifnum, 1)) {
    printf("%s: CDC-ECM: Unable to select alternate setting\n",
           ud->ud_dev.d_name);
    free(e);
    return NULL;
  }

  e->ud = ud;
  e->out_mps = out_ed->wMaxPacketSize;
  e->in = usb_host_pipe_open(ud, in_ed);
  e->out = usb_host_pipe_open(ud, out_ed);
  if(e->in == NULL || e->out == NULL) {
    if(e->in)
      usb_host_pipe_close(ud, e->in);
    if(e->out)
      usb_host_pipe_close(ud, e->out);
    free(e);
    return NULL;
  }

  usb_host_control(ud, 0x21, CDC_SET_ETHERNET_PACKET_FILTER,
                   CDC_PACKET_TYPE_DIRECTED | CDC_PACKET_TYPE_BROADCAST |
                   CDC_PACKET_TYPE_ALL_MULTICAST,
                   id->bInterfaceNumber, NULL, 0);

  ud->ud_claimed |= 1 << data_ifnum;

  // The host controller driver does cache maintenance
  for(int i = 0; i < ECM_RX_BUFS; i++) {
    e->rx_dma[i] = xalloc(ECM_BUF_SIZE, 64, MEM_TYPE_DMA);
    e->rx_xfer[i].ux_done = ecm_rx_done;
    e->rx_xfer[i].ux_opaque = e;
  }
  e->tx_dma = xalloc(ECM_BUF_SIZE, 64, MEM_TYPE_DMA);
  e->tx_xfer.ux_done = ecm_tx_done;
  e->tx_xfer.ux_opaque = e;

  e->task.nt_cb = ecm_task_cb;

  snprintf(e->name, sizeof(e->name), "ecm%d", ecm_unit++);
  e->eni.eni_output = ecm_output;
  ether_netif_init(&e->eni, e->name, &ecm_device_class);
  ether_netif_attach(&e->eni);

  const uint8_t *m = e->eni.eni_addr;
  printf("%s: CDC-ECM %s (interfaces %d,%d) "
         "%02x:%02x:%02x:%02x:%02x:%02x\n",
         ud->ud_dev.d_name, e->name, id->bInterfaceNumber, data_ifnum,
         m[0], m[1], m[2], m[3], m[4], m[5]);

  int q = irq_forbid(IRQ_LEVEL_IO);
  ecm_rx_start(e);
  irq_permit(q);

  // The device reports the link state on the interrupt endpoint, which
  // we don't poll (no interrupt pipes yet). Assume the link is up
  net_task_raise(&e->eni.eni_ni.ni_task, NETIF_TASK_STATUS_UP);
  return e;
}


static void
ecm_detach(usb_host_device_t *ud, void *instance)
{
  usb_host_ecm_t *e = instance;

  int q = irq_forbid(IRQ_LEVEL_IO);
  e->gone = 1;
  irq_permit(q);

  // Pending transfers complete (with an error) before these return
  usb_host_pipe_close(ud, e->in);
  usb_host_pipe_close(ud, e->out);

  printf("%s: CDC-ECM %s detached\n", ud->ud_dev.d_name, e->name);

  // Runs ecm_disable() on the network thread and drops the device,
  // which frees us once nobody else holds a reference
  netif_detach(&e->eni.eni_ni);
}


static usb_host_driver_t ecm_driver = {
  .uhd_probe = ecm_probe,
  .uhd_detach = ecm_detach,
};


void
usb_host_ecm_register(void)
{
  usb_host_driver_register(&ecm_driver);
}
