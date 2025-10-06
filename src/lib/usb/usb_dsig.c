#include <mios/stream.h>
#include <mios/task.h>
#include <mios/fifo.h>
#include <mios/cli.h>

#include <sys/param.h>

#include <assert.h>
#include <string.h>
#include <stdlib.h>

#include <net/can/can.h>
#include <usb/usb_desc.h>
#include <usb/usb.h>

#include "net/netif.h"
#include "net/dsig.h"

#include "irq.h"

typedef struct usb_dsig {

  can_netif_t cni;

  uint8_t rx_nak;
  uint8_t tx_on;
  uint8_t um_usb_sub_class;
  uint8_t tx_queue_len;
  uint8_t cnt;

  usb_interface_t *iface;

  struct pbuf_queue tx_queue;

  pbuf_t *rx_pbuf;

} usb_dsig_t;



static size_t
dsig_gen_desc(void *ptr, void *opaque, int iface_index)
{
  usb_dsig_t *ud = opaque;
  return usb_gen_iface_desc(ptr, iface_index, 2, 255, ud->um_usb_sub_class);
}


static void
buffer_alloc(usb_dsig_t *ud)
{
  // IRQ_LEVEL_NET must be blocked

  void *buf = pbuf_data_get(0);
  if(buf == NULL) {
    ud->rx_pbuf = NULL;
    return;
  }

  pbuf_t *pb = pbuf_get(0);
  if(pb == NULL) {
    pbuf_data_put(buf);
  } else {
    pb->pb_data = buf;

    usb_ep_t *ue = &ud->iface->ui_endpoints[0]; // OUT
    if(ue->ue_vtable != NULL)
      ue->ue_vtable->cnak(ue->ue_dev, ue);
  }
  ud->rx_pbuf = pb;
}



static void
dsig_rx(device_t *d, usb_ep_t *ue, uint32_t bytes, uint32_t flags)
{
  usb_dsig_t *ud = ue->ue_iface_aux;

  pbuf_t *pb = ud->rx_pbuf;
  assert(pb != NULL);

  ue->ue_vtable->read(d, ue, pb->pb_data, PBUF_DATA_SIZE, 2, bytes);

  if(bytes > PBUF_DATA_SIZE - 2)
    return; // Too large

  const uint8_t *hdr = pb->pb_data  + 2;
  uint32_t id = hdr[0] | ((hdr[1] & 0xf) << 8);

  if((hdr[1] & 0xc0) != 0xc0) {
    return; // Fragmented packets not supported
  }

  pb->pb_flags = PBUF_SOP | PBUF_EOP;
  pb->pb_pktlen = bytes - 2;
  pb->pb_offset = 0;
  pb->pb_buflen = bytes - 2;
  uint32_t *u32 = pbuf_data(pb, 0);
  *u32 = id;

  STAILQ_INSERT_TAIL(&ud->cni.cni_ni.ni_rx_queue, pb, pb_link);
  netif_wakeup(&ud->cni.cni_ni);

  buffer_alloc(ud);
}


static void
do_tx(usb_dsig_t *ud)
{
  pbuf_t *pb = pbuf_splice(&ud->tx_queue);
  if(pb == NULL) {
    ud->tx_on = 0;
    return;
  }
  assert(ud->tx_queue_len);
  ud->tx_queue_len--;
  usb_ep_t *ue = &ud->iface->ui_endpoints[1]; // IN

  if(ue->ue_running) {
    ue->ue_vtable->write(ue->ue_dev, ue, pb->pb_data + pb->pb_offset,
                         pb->pb_buflen);
    ud->tx_on = 1;
  }
  pbuf_free_irq_blocked(pb);
}


static void
dsig_txco(device_t *d, usb_ep_t *ue, uint32_t bytes, uint32_t flags)
{
  usb_dsig_t *ud = ue->ue_iface_aux;
  do_tx(ud);
}


static void
dsig_tx_reset(device_t *d, usb_ep_t *ue)
{
  usb_dsig_t *um = ue->ue_iface_aux;

  if(ue->ue_running) {
    do_tx(um);
  }
}


static pbuf_t *
usb_dsig_output(struct can_netif *cni, pbuf_t *pb, uint32_t id)
{
  usb_dsig_t *ud = (usb_dsig_t *)cni;

  if(id > 0xfff)
    return pb; // Out of range

  // Prepend header
  pb = pbuf_prepend(pb, 2, 0, 0);
  if(pb == NULL)
    return NULL;

  uint8_t *hdr = pbuf_data(pb, 0);

  hdr[0] = id;
  hdr[1] =
    ((id >> 8) & 0xf) |
    0xc0 |  // Hardcode both first-frag and last-frag for now
    ((ud->cnt & 0x3) << 4);

  // Make a single contig pbuf
  if(pbuf_pullup(pb, pb->pb_pktlen)) {
    return pb;
  }

  if(pb->pb_buflen > 64)
    return pb;

  int q = irq_forbid(IRQ_LEVEL_NET);

  if(ud->tx_queue_len < 4) {
    STAILQ_INSERT_TAIL(&ud->tx_queue, pb, pb_link);
    ud->tx_queue_len++;
    ud->cnt++;

    pb = NULL;
    if(!ud->tx_on) {
      do_tx(ud);
    }
  }
  irq_permit(q);
  return pb;
}


static void
buffers_avail(struct netif *ni)
{
  usb_dsig_t *ud = (usb_dsig_t *)ni;

  if(ud->rx_pbuf != NULL)
    return;

  int q = irq_forbid(IRQ_LEVEL_NET);
  buffer_alloc(ud);
  irq_permit(q);
}


static void
usb_dsig_print_info(struct device *dev, struct stream *st)
{
  //  usb_dsig_t *um = (usb_dsig_t *)dev;
  //  dsig_print_info(&ud->um_mni, st);
}


static const device_class_t usb_dsig_device_class = {
  .dc_print_info = usb_dsig_print_info
};



void
usb_dsig_create(struct usb_interface_queue *q,
                uint8_t usb_sub_class)
{
  usb_dsig_t *um = calloc(1, sizeof(usb_dsig_t));
  um->um_usb_sub_class = usb_sub_class;

  STAILQ_INIT(&um->tx_queue);

  um->iface = usb_alloc_interface(q, dsig_gen_desc, um, 2, "usb-dsig");

  usb_init_endpoint(&um->iface->ui_endpoints[0],
                    um, dsig_rx, NULL,
                    USB_ENDPOINT_BULK, 0x0, 0x1, 64);

  usb_init_endpoint(&um->iface->ui_endpoints[1],
                    um, dsig_txco, dsig_tx_reset,
                    USB_ENDPOINT_BULK, 0x80, 0x1, 64);

  um->cni.cni_output = usb_dsig_output;
  um->cni.cni_ni.ni_buffers_avail = buffers_avail;

  can_netif_attach(&um->cni, "usbdsig",
                   &usb_dsig_device_class, NULL);
}
