#include <mios/mios.h>

#include <usb/usb.h>
#include <usb/usb_desc.h>

#include <string.h>
#include <unistd.h>

#include "net/net_task.h"

// DFU class/subclass/protocol for Runtime mode
#define DFU_IFACE_CLASS     0xfe  // Application Specific
#define DFU_IFACE_SUBCLASS  0x01  // DFU
#define DFU_IFACE_PROTO_RT  0x01  // Runtime

// DFU Functional Descriptor type
#define USB_DESC_TYPE_DFU_FUNCTIONAL 0x21

// DFU bmAttributes bits
#define DFU_ATTR_CAN_DNLOAD    (1 << 0)
#define DFU_ATTR_CAN_UPLOAD    (1 << 1)
#define DFU_ATTR_MANIFESTATION_TOLERANT (1 << 2)
#define DFU_ATTR_WILL_DETACH   (1 << 3)

// DFU class-specific requests
#define DFU_REQ_DETACH  0x00

struct dfu_functional_descriptor {
  uint8_t  bLength;
  uint8_t  bDescriptorType;
  uint8_t  bmAttributes;
  uint16_t wDetachTimeOut;
  uint16_t wTransferSize;
  uint16_t bcdDFUVersion;
} __attribute__((packed));

struct dfu_runtime_desc {
  struct usb_interface_descriptor iface;
  struct dfu_functional_descriptor func;
} __attribute__((packed));

static timer_t detach_timer;

static void
dfu_detach_timer_cb(void *opaque, uint64_t now)
{
  dfu();
}

static size_t
dfu_rt_gen_desc(void *ptr, void *opaque, int iface_index)
{
  if(ptr != NULL) {
    struct dfu_runtime_desc *d = ptr;

    d->iface.bLength = sizeof(struct usb_interface_descriptor);
    d->iface.bDescriptorType = USB_DESC_TYPE_INTERFACE;
    d->iface.bInterfaceNumber = iface_index;
    d->iface.bAlternateSetting = 0;
    d->iface.bNumEndpoints = 0;
    d->iface.bInterfaceClass = DFU_IFACE_CLASS;
    d->iface.bInterfaceSubClass = DFU_IFACE_SUBCLASS;
    d->iface.bInterfaceProtocol = DFU_IFACE_PROTO_RT;
    d->iface.iInterface = 0;

    d->func.bLength = sizeof(struct dfu_functional_descriptor);
    d->func.bDescriptorType = USB_DESC_TYPE_DFU_FUNCTIONAL;
    d->func.bmAttributes = DFU_ATTR_CAN_DNLOAD | DFU_ATTR_WILL_DETACH;
    d->func.wDetachTimeOut = 1000;   // ms
    d->func.wTransferSize = 2048;
    d->func.bcdDFUVersion = 0x0110;  // DFU 1.1
  }
  return sizeof(struct dfu_runtime_desc);
}

static void
dfu_rt_iface_cfg(void *opaque, int request, int value)
{
  if(request == DFU_REQ_DETACH)
    net_timer_arm(&detach_timer, clock_get_irq_blocked() + 100000);
}

void
usb_dfu_runtime_create(struct usb_interface_queue *q)
{
  detach_timer.t_cb = dfu_detach_timer_cb;

  usb_interface_t *ui = usb_alloc_interface(q, dfu_rt_gen_desc, NULL,
                                            0, "dfu-rt");
  ui->ui_iface_cfg = dfu_rt_iface_cfg;
}
