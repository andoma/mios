#pragma once

#include <stdint.h>

#include <mios/device.h>
#include <mios/error.h>

typedef struct usb_ep usb_ep_t;

typedef struct {

  void (*read)(device_t *dev, usb_ep_t *ue,
               uint8_t *buf, size_t buf_size,
               size_t buf_offset, size_t bytes);

  error_t (*write)(device_t *dev, usb_ep_t *ep,
                   const uint8_t *buf, size_t buf_size);

  void (*write1)(device_t *dev, usb_ep_t *ep,
                 size_t len, uint8_t (*getu8)(void *opaque), void *opaque);

  void (*cnak)(device_t *dev, usb_ep_t *ep);

  int (*avail_bytes)(device_t *dev, usb_ep_t *ep);

} usb_ctrl_vtable_t;



struct usb_ep {

  // Initialized by interface
  void *ue_iface_aux;

  // Initialized by interface
  // Called by controller when RX happens or TX is done
  error_t (*ue_completed)(device_t *d, struct usb_ep *ep,
                          uint32_t status, uint32_t bytes);

  // Initialized by interface
  void (*ue_reset)(device_t *d, struct usb_ep *ep);

  // Set by controller
  device_t *ue_dev;

  // Set by controller
  const usb_ctrl_vtable_t *ue_vtable;

  // USB_ENDPOINT_... defines
  uint8_t ue_endpoint_type;

  // Initialzed to 0x80 (IN) or 0x00 (OUT) by interface, lower 7 bits
  // assigned by controller
  uint8_t ue_address;

  // Set by controller when running. XXX: Change to reflect nak, etc?
  uint8_t ue_running;

  // Initialized by interface
  uint8_t ue_interval;

  // Initialized by interface
  uint16_t ue_max_packet_size;

  const char *ue_name;

  // Stats
  uint32_t ue_num_packets;
  uint32_t ue_num_drops;

};

typedef size_t (usb_gen_desc_t)(void *buf, void *opaque, int iface_index);


STAILQ_HEAD(usb_interface_queue, usb_interface);

typedef struct usb_interface {

  STAILQ_ENTRY(usb_interface) ui_link;

  usb_gen_desc_t *ui_gen_desc;
  void *ui_opaque;
  const char *ui_name;

  size_t ui_num_endpoints;
  usb_ep_t ui_endpoints[0];

} usb_interface_t;



size_t
usb_gen_iface_desc(void *ptr, int iface_index, int num_endpoints,
                   int class, int subclass);

usb_interface_t *usb_alloc_interface(struct usb_interface_queue *q,
                                     usb_gen_desc_t *gen_desc,
                                     void *opaque,
                                     size_t num_endpoints,
                                     const char *name);


static inline void
usb_init_endpoint(usb_ep_t *ue, void *aux,
                  error_t (*completed)(device_t *d, struct usb_ep *ep,
                                          uint32_t status, uint32_t bytes),
                  void (*reset)(device_t *d, struct usb_ep *ep),
                  int type,
                  int address,
                  int interval,
                  int max_packet_size)
{
  ue->ue_iface_aux = aux;
  ue->ue_completed = completed;
  ue->ue_reset = reset;
  ue->ue_endpoint_type = type;
  ue->ue_address = address;
  ue->ue_interval = interval;
  ue->ue_max_packet_size = max_packet_size;
}


void usb_cdc_create(struct usb_interface_queue *q);

void usb_mbus_create(struct usb_interface_queue *q, uint8_t local_addr,
                     uint8_t usb_sub_class);
