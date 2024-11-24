#include <mios/stream.h>
#include <mios/task.h>
#include <mios/fifo.h>
#include <mios/cli.h>
#include <mios/mios.h>

#include <sys/param.h>

#include <assert.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>

#include <usb/usb_desc.h>
#include <usb/usb.h>

#include "irq.h"

typedef struct usb_cdc {

  stream_t s;

  uint8_t line_state;
  uint8_t flags;
  uint8_t rx_nak;
  uint8_t tx_on;

  task_waitable_t rx_waitq;
  FIFO_DECL(rx_fifo, 32);

  task_waitable_t tx_waitq;
  FIFO_DECL(tx_fifo, 16);

  usb_interface_t *comm_iface;
  usb_interface_t *data_iface;

} usb_cdc_t;






struct usb_cdc_header_desc {
  uint8_t bFunctionLength;
  uint8_t bDescriptorType;
  uint8_t bDescriptorSubType;
  uint16_t bcdCDC;
} __attribute__ ((packed));


struct usb_cdc_call_mgmt_desc {
  uint8_t bFunctionLength;
  uint8_t bDescriptorType;
  uint8_t bDescriptorSubType;
  uint8_t bmCapabilities;
  uint8_t bDataInterface;
} __attribute__ ((packed));


struct usb_cdc_acm_desc {
  uint8_t bFunctionLength;
  uint8_t bDescriptorType;
  uint8_t bDescriptorSubType;
  uint8_t bmCapabilities;
} __attribute__ ((packed));


struct usb_cdc_union_desc {
  uint8_t bFunctionLength;
  uint8_t bDescriptorType;
  uint8_t bDescriptorSubType;
  uint8_t bMasterInterface0;
  uint8_t bSlaveInterface0;
} __attribute__ ((packed));




struct cdc_comm_desc {
  struct usb_iad_descriptor iad;
  struct usb_interface_descriptor iface;
  struct usb_cdc_header_desc hdr;
  struct usb_cdc_call_mgmt_desc mgmt;
  struct usb_cdc_acm_desc  acm;
  struct usb_cdc_union_desc u;
} __attribute__((packed));



static const struct cdc_comm_desc cdc_comm_desc = {
  .iad = {
    .bLength = sizeof(struct usb_iad_descriptor),
    .bDescriptorType = USB_DESC_TYPE_INTERFACE_ASSOC,
    .bFirstInterface = 0,
    .bInterfaceCount = 2,
    .bFunctionClass = 2,     // CDC
    .bFunctionSubClass = 2,  // ACM
    .bFunctionProtocol = 0,
    .iFunction = 0,
  },

  .iface = {
    .bLength = sizeof(struct usb_interface_descriptor),
    .bDescriptorType = USB_DESC_TYPE_INTERFACE,
    .bInterfaceNumber = 0,
    .bAlternateSetting = 0,
    .bNumEndpoints = 1,
    .bInterfaceClass = 2,    // CDC
    .bInterfaceSubClass = 2, // ACM
    .bInterfaceProtocol = 0,
    .iInterface = 0,
  },
  .hdr = {
    .bFunctionLength = sizeof(struct usb_cdc_header_desc),
    .bDescriptorType = USB_DESC_TYPE_CS_INTERFACE,
    .bDescriptorSubType = 0,
    .bcdCDC = 0x110,
  },
  .mgmt = {
    .bFunctionLength = sizeof(struct usb_cdc_call_mgmt_desc),
    .bDescriptorType = USB_DESC_TYPE_CS_INTERFACE,
    .bDescriptorSubType = 1, // Call management
    .bmCapabilities = 0,
    .bDataInterface = 1,
  },
  .acm = {
    .bFunctionLength = sizeof(struct usb_cdc_acm_desc),
    .bDescriptorType = USB_DESC_TYPE_CS_INTERFACE,
    .bDescriptorSubType = 2, // CDC_ACM
    .bmCapabilities = 2      // CAP_LINE
  },
  .u = {
    .bFunctionLength = sizeof(struct usb_cdc_union_desc),
    .bDescriptorType = USB_DESC_TYPE_CS_INTERFACE,
    .bDescriptorSubType = 6,
    .bMasterInterface0 = 0,
    .bSlaveInterface0 = 1,
  }
};

#define USB_CDC_SET_CONTROL_LINE_STATE		0x22
#define USB_CDC_LINE_STATE_DTR 0x1
#define USB_CDC_LINE_STATE_RTS 0x2


static size_t
cdc_gen_comm_desc(void *ptr, void *opaque, int iface_index)
{
  if(ptr != NULL) {

    memcpy(ptr, &cdc_comm_desc, sizeof(cdc_comm_desc));
    struct cdc_comm_desc *ccd = ptr;

    ccd->iad.bFirstInterface = iface_index;
    ccd->iface.bInterfaceNumber = iface_index;
    ccd->mgmt.bDataInterface = iface_index + 1;
    ccd->u.bMasterInterface0 = iface_index;
    ccd->u.bSlaveInterface0 = iface_index + 1;
  }
  return sizeof(struct cdc_comm_desc);
}



static size_t
cdc_gen_data_desc(void *ptr, void *opaque, int iface_index)
{
  return usb_gen_iface_desc(ptr, iface_index, 2, 10, 0);
}



static void
cdc_rx_pkt(device_t *d, usb_ep_t *ue, usb_cdc_t *uc, int bytes)
{
  uint8_t avail = fifo_avail(&uc->rx_fifo);
  assert(avail >= bytes);

  ue->ue_vtable->read(d, ue, uc->rx_fifo.buf, sizeof(uc->rx_fifo.buf),
                      uc->rx_fifo.wrptr, bytes);

  uc->rx_fifo.wrptr += bytes;
}


static void
cdc_rx(device_t *d, usb_ep_t *ue, uint32_t bytes, uint32_t flags)
{
  if(flags)
    panic("cdc_rx_pkt: flags:%x", flags);
  usb_cdc_t *uc = ue->ue_iface_aux;

  if(uc->flags & USB_CDC_DISCARD_RX) {
    ue->ue_vtable->cnak(ue->ue_dev, ue);
    return;
  }
  cdc_rx_pkt(d, ue, uc, bytes);
  uc->rx_nak = 1;
  task_wakeup(&uc->rx_waitq, 0);
}


static uint8_t
cdc_tx_fifo_getch(void *opaque)
{
  usb_cdc_t *uc = opaque;
  return fifo_rd(&uc->tx_fifo);
}



static void
cdc_tx(device_t *d, usb_ep_t *ue, usb_cdc_t *uc)
{
  const int avail_bytes = ue->ue_vtable->avail_bytes(d, ue);

  size_t len = MIN(avail_bytes, fifo_used(&uc->tx_fifo));

  if(len == 0) {
    uc->tx_on = 0;
    return;
  }
  uc->tx_on = 1;

  ue->ue_vtable->write1(d, ue, len, cdc_tx_fifo_getch, uc);

  task_wakeup(&uc->tx_waitq, 0);
}



static void
cdc_txco(device_t *d, usb_ep_t *ue, uint32_t bytes, uint32_t flags)
{
  cdc_tx(d, ue, ue->ue_iface_aux);
}



static void
cdc_tx_reset(device_t *d, usb_ep_t *ue)
{
  usb_cdc_t *uc = ue->ue_iface_aux;

  if(ue->ue_running && !fifo_is_empty(&uc->tx_fifo)) {
    cdc_tx(d, ue, uc);
  }
}



static task_waitable_t *
cdc_poll(struct stream *s, poll_type_t type)
{
  usb_cdc_t *uc = (usb_cdc_t *)s;

  irq_forbid(IRQ_LEVEL_NET);

  if(type == POLL_STREAM_WRITE) {
    if(!fifo_is_full(&uc->tx_fifo))
      return NULL;
    return &uc->tx_waitq;
  } else {
    if(!fifo_is_empty(&uc->rx_fifo))
      return NULL;
    return &uc->rx_waitq;
  }
}


static ssize_t
cdc_read(struct stream *s, void *buf, size_t size, size_t required)
{
  usb_cdc_t *uc = (usb_cdc_t *)s;
  usb_ep_t *ue = &uc->data_iface->ui_endpoints[1]; // OUT

  int q = irq_forbid(IRQ_LEVEL_NET);

  uint8_t *d = buf;

  for(size_t i = 0; i < size; i++) {
    while(fifo_is_empty(&uc->rx_fifo)) {

      if(uc->rx_nak) {
        uc->rx_nak = 0;
        ue->ue_vtable->cnak(ue->ue_dev, ue);
      }

      if(i >= required) {
        irq_permit(q);
        return i;
      }
      task_sleep(&uc->rx_waitq);
    }

    d[i] = fifo_rd(&uc->rx_fifo);
  }
  irq_permit(q);
  return size;
}




static ssize_t
cdc_write(struct stream *s, const void *buf, size_t size, int flags)
{
  if(size == 0)
    return 0;

  ssize_t written = 0;
  usb_cdc_t *uc = (usb_cdc_t *)s;
  usb_ep_t *ue = &uc->data_iface->ui_endpoints[0]; // IN

  const uint8_t *b = buf;
  int q = irq_forbid(IRQ_LEVEL_NET);

  if(flags & STREAM_WRITE_WAIT_DTR) {
    while(!(uc->line_state & USB_CDC_LINE_STATE_DTR)) {
      task_sleep(&uc->tx_waitq);

      /* Linux have this strange idea of echoing back characters
         until the TTY is set in RAW mode.
         Thus we sleep for a while to give it a chance to get
         things in order.
      */
      usleep(10000);
    }
  }

  for(size_t i = 0; i < size; i++) {
    while(fifo_is_full(&uc->tx_fifo)) {
      if(flags & STREAM_WRITE_NO_WAIT)
        goto done;
      task_sleep(&uc->tx_waitq);
    }

    fifo_wr(&uc->tx_fifo, b[i]);
    written++;
    if(ue->ue_running && !uc->tx_on) {
      cdc_tx(ue->ue_dev, ue, uc);
      uc->tx_on = 1;
    }
  }
 done:
  irq_permit(q);
  return written;
}


__attribute__((noreturn))
static void *
cdc_shell_thread(void *arg)
{
  usb_cdc_t *cdc = arg;

  while(1) {
    cli_on_stream(&cdc->s, '>');
  }
}


static void
usb_cdc_iface_cfg(void *opaque, int req, int value)
{
  usb_cdc_t *cdc = opaque;
  if(req == USB_CDC_SET_CONTROL_LINE_STATE) {
    cdc->line_state = value;
    task_wakeup(&cdc->tx_waitq, 1);
  }
}

struct stream *
usb_cdc_create_stream(struct usb_interface_queue *q, int flags)
{
  usb_cdc_t *cdc = calloc(1, sizeof(usb_cdc_t));
  cdc->s.read = cdc_read;
  cdc->s.write = cdc_write;
  cdc->s.poll = cdc_poll;
  cdc->flags = flags;
  task_waitable_init(&cdc->rx_waitq, "cdcrx");
  task_waitable_init(&cdc->tx_waitq, "cdctx");

  cdc->comm_iface =
    usb_alloc_interface(q, cdc_gen_comm_desc, cdc, 1, "cdc-comm");

  cdc->comm_iface->ui_iface_cfg = usb_cdc_iface_cfg;

  usb_init_endpoint(&cdc->comm_iface->ui_endpoints[0],
                    NULL, NULL, NULL,
                    USB_ENDPOINT_INTERRUPT, 0x80, 0xff, 8);

  cdc->data_iface =
    usb_alloc_interface(q, cdc_gen_data_desc, cdc, 2, "cdc-data");

  usb_init_endpoint(&cdc->data_iface->ui_endpoints[0],
                    cdc, cdc_txco, cdc_tx_reset,
                    USB_ENDPOINT_BULK, 0x80, 1, 64);

  usb_init_endpoint(&cdc->data_iface->ui_endpoints[1],
                    cdc, cdc_rx, NULL,
                    USB_ENDPOINT_BULK, 0, 1, 64);

  return &cdc->s;
}


void
usb_cdc_create_shell(struct usb_interface_queue *q)
{
  struct stream *s = usb_cdc_create_stream(q, 0);
  thread_create_shell(cdc_shell_thread, s, "cdc-cli", s);
}
