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
  uint32_t rx_accum; // bytes accumulated so far for the in-progress OUT transfer

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

  if(2 + ud->rx_accum + bytes <= PBUF_DATA_SIZE) {
    ue->ue_vtable->read(d, ue, pb->pb_data, PBUF_DATA_SIZE,
                        2 + ud->rx_accum, bytes);
    ud->rx_accum += bytes;
  } else {
    // Oversized transfer -- drain the packet but don't let rx_accum
    // land back at a size we'd mistake for valid below.
    ud->rx_accum = PBUF_DATA_SIZE;
  }

  // A logical OUT transfer can span multiple USB packets: a packet of
  // exactly ue_max_packet_size means more data may follow. Only a
  // short (or zero-length) packet, per USB bulk semantics, ends the
  // transfer. Keep accumulating into the same pbuf until then.
  if(bytes == ue->ue_max_packet_size) {
    ue->ue_vtable->cnak(ue->ue_dev, ue);
    return;
  }

  const uint32_t total = ud->rx_accum;
  ud->rx_accum = 0;

  const uint8_t *hdr = pb->pb_data + 2;
  uint32_t id = hdr[0] | ((hdr[1] & 0xf) << 8);

  if(total < 2 || total > PBUF_DATA_SIZE - 2 || (hdr[1] & 0xc0) != 0xc0) {
    // Invalid, oversized, or a bare trailing ZLP -- reuse the buffer,
    // just re-arm for the next transfer.
    ue->ue_vtable->cnak(ue->ue_dev, ue);
    return;
  }

  pb->pb_flags = PBUF_SOP | PBUF_EOP;
  // can_input() expects [4-byte LE id][payload] and drops the first 4
  // bytes to recover the payload -- pktlen must span both, not just
  // the (total - 2) actual USB-dsig payload.
  pb->pb_pktlen = total + 2;
  pb->pb_offset = 0;
  pb->pb_buflen = total + 2;
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

  const size_t total = pb->pb_buflen;

  // A logical message can exceed one USB max-size packet (64 bytes) --
  // this endpoint's hardware sends exactly one packet per write() (see
  // stm32_otg.c's ep_start(), PKTCNT hardcoded to 1), so split into
  // <=64-byte fragments queued separately and drained one at a time by
  // do_tx()/dsig_txco(). Only the first fragment carries the 2-byte
  // dsig-over-usb header (already prepended above); the rest is pure
  // continuation payload, matching what dsig_rx() expects on the
  // receiving end. A message that's an exact multiple of 64 needs a
  // trailing zero-length fragment to mark its end, per USB bulk
  // semantics (mirrors the fix on the host's dsig_usb_tx side).
  size_t nfrags = (total + 63) / 64;
  if(nfrags == 0 || total % 64 == 0)
    nfrags++;

  int q = irq_forbid(IRQ_LEVEL_NET);

  if(ud->tx_queue_len + nfrags > 4) {
    irq_permit(q);
    return pb;
  }

  for(size_t off = 0; off <= total; off += 64) {
    size_t len = MIN(total - off, 64);

    pbuf_t *frag = pbuf_get(0);
    if(frag == NULL)
      break;
    void *buf = pbuf_data_get(0);
    if(buf == NULL) {
      pbuf_put(frag);
      break;
    }
    frag->pb_data = buf;
    frag->pb_offset = 0;
    frag->pb_buflen = len;
    frag->pb_pktlen = len;
    frag->pb_flags = PBUF_SOP | PBUF_EOP;
    if(len)
      memcpy(buf, pbuf_data(pb, off), len);

    STAILQ_INSERT_TAIL(&ud->tx_queue, frag, pb_link);
    ud->tx_queue_len++;

    if(off + 64 > total)
      break; // just queued the final (possibly zero-length) fragment
  }
  ud->cnt++;

  if(!ud->tx_on) {
    do_tx(ud);
  }
  irq_permit(q);

  pbuf_free_irq_blocked(pb);
  return NULL;
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
  .dc_class_name = "usb-dsig",
  .dc_print_info = usb_dsig_print_info
};



void
usb_dsig_create(struct usb_interface_queue *q,
                uint8_t usb_sub_class,
                const struct dsig_filter *output_filter)
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
                   &usb_dsig_device_class, output_filter);
}
