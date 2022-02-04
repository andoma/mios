
#include <mios/stream.h>
#include <mios/task.h>
#include <mios/fifo.h>
#include <mios/cli.h>

#include <sys/param.h>

#include <assert.h>
#include <string.h>
#include <stdlib.h>

#include <usb/usb_desc.h>
#include <usb/usb.h>

#include "net/netif.h"
#include "net/mbus/mbus.h"

#include "irq.h"

typedef struct usb_mbus {

  mbus_netif_t um_mni;

  uint8_t rx_nak;
  uint8_t tx_on;
  uint8_t um_usb_sub_class;

  usb_interface_t *iface;

  struct pbuf_queue tx_queue;

  pbuf_t *rx_pbuf;

} usb_mbus_t;



static size_t
mbus_gen_desc(void *ptr, void *opaque, int iface_index)
{
  usb_mbus_t *um = opaque;
  return usb_gen_iface_desc(ptr, iface_index, 2, 255, um->um_usb_sub_class);
}


static void
buffer_alloc(usb_mbus_t *um)
{
  // IRQ_LEVEL_NET must be blocked

  void *buf = pbuf_data_get(0);
  if(buf == NULL) {
    um->rx_pbuf = NULL;
    return;
  }

  pbuf_t *pb = pbuf_get(0);
  if(pb == NULL) {
    pbuf_data_put(buf);
  } else {
    pb->pb_data = buf;

    usb_ep_t *ue = &um->iface->ui_endpoints[0]; // OUT
    if(ue->ue_vtable != NULL)
      ue->ue_vtable->cnak(ue->ue_dev, ue);
  }
  um->rx_pbuf = pb;
}



static error_t
mbus_rx(device_t *d, usb_ep_t *ue, uint32_t status, uint32_t bytes)
{
  usb_mbus_t *um = ue->ue_iface_aux;

  if(status != 2)
    return 0;

  pbuf_t *pb = um->rx_pbuf;
  assert(pb != NULL);

  ue->ue_vtable->read(d, ue, pb->pb_data, PBUF_DATA_SIZE, 0, bytes);

  pb->pb_flags = PBUF_SOP | PBUF_EOP;
  pb->pb_pktlen = bytes;
  pb->pb_offset = 0;
  pb->pb_buflen = bytes;
  STAILQ_INSERT_TAIL(&um->um_mni.mni_ni.ni_rx_queue, pb, pb_link);
  netif_wakeup(&um->um_mni.mni_ni);

  buffer_alloc(um);
  return 0;
}


static void
do_tx(usb_mbus_t *um)
{
  pbuf_t *pb = pbuf_splice(&um->tx_queue);
  if(pb == NULL) {
    um->tx_on = 0;
    return;
  }
  usb_ep_t *ue = &um->iface->ui_endpoints[1]; // IN

  if(ue->ue_running) {
    ue->ue_vtable->write(ue->ue_dev, ue, pb->pb_data, pb->pb_buflen);
    um->tx_on = 1;
  }
  pbuf_free_irq_blocked(pb);
}


static error_t
mbus_txco(device_t *d, usb_ep_t *ue, uint32_t status, uint32_t bytes)
{
  usb_mbus_t *um = ue->ue_iface_aux;
  do_tx(um);
  return 0;
}


static void
mbus_tx_reset(device_t *d, usb_ep_t *ue)
{
  usb_mbus_t *um = ue->ue_iface_aux;

  if(ue->ue_running) {
    do_tx(um);
  }
}


static void
usb_mbus_output(struct mbus_netif *mni, pbuf_t *pb)
{
  usb_mbus_t *um = (usb_mbus_t *)mni;

  if(pb->pb_buflen != pb->pb_pktlen) {
    printf("usb_mbus: Warning buflen %d != pktlen %d\n",
           pb->pb_buflen, pb->pb_pktlen);
  }

  assert((pb->pb_flags & (PBUF_SOP | PBUF_EOP)) == (PBUF_SOP | PBUF_EOP));

  int q = irq_forbid(IRQ_LEVEL_NET);

  STAILQ_INSERT_TAIL(&um->tx_queue, pb, pb_link);

  if(!um->tx_on)
    do_tx(um);

  irq_permit(q);
}


static void
buffers_avail(struct netif *ni)
{
  usb_mbus_t *um = (usb_mbus_t *)ni;

  if(um->rx_pbuf != NULL)
    return;

  int q = irq_forbid(IRQ_LEVEL_NET);
  buffer_alloc(um);
  irq_permit(q);
}




void
usb_mbus_create(struct usb_interface_queue *q, uint8_t local_addr,
                uint8_t usb_sub_class)
{
  usb_mbus_t *um = calloc(1, sizeof(usb_mbus_t));
  um->um_usb_sub_class = usb_sub_class;

  STAILQ_INIT(&um->tx_queue);

  um->iface = usb_alloc_interface(q, mbus_gen_desc, um, 2, "usb-mbus");

  usb_init_endpoint(&um->iface->ui_endpoints[0],
                    um, mbus_rx, NULL,
                    USB_ENDPOINT_BULK, 0x0, 0x1, 32);

  usb_init_endpoint(&um->iface->ui_endpoints[1],
                    um, mbus_txco, mbus_tx_reset,
                    USB_ENDPOINT_BULK, 0x80, 0x1, 32);

  um->um_mni.mni_hdr_len = 1;
  um->um_mni.mni_output = usb_mbus_output;
  um->um_mni.mni_ni.ni_buffers_avail = buffers_avail;
  um->um_mni.mni_ni.ni_mtu = 32;

  mbus_netif_attach(&um->um_mni, "usbmbus", local_addr, 0);
}
