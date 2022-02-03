#include <usb/usb.h>
#include <usb/usb_desc.h>

#include <stdlib.h>

size_t
usb_gen_iface_desc(void *ptr, int iface_index, int num_endpoints,
                   int class, int subclass)
{
  if(ptr != NULL) {
    struct usb_interface_descriptor *uid = ptr;
    uid->bLength = sizeof(struct usb_interface_descriptor);
    uid->bDescriptorType = USB_DESC_TYPE_INTERFACE;
    uid->bInterfaceNumber = iface_index;
    uid->bNumEndpoints = num_endpoints;
    uid->bInterfaceClass = class;
    uid->bInterfaceSubClass = subclass;
  }
  return sizeof(struct usb_interface_descriptor);
}


usb_interface_t *
usb_alloc_interface(struct usb_interface_queue *q,
                    usb_gen_desc_t *gen_desc, void *opaque,
                    size_t num_endpoints)
{
  usb_interface_t *ui = calloc(1, sizeof(usb_interface_t) +
                               num_endpoints * sizeof(usb_ep_t));

  ui->ui_gen_desc = gen_desc;
  ui->ui_opaque = opaque;
  ui->ui_num_endpoints = num_endpoints;

  STAILQ_INSERT_TAIL(q, ui, ui_link);
  return ui;
}
