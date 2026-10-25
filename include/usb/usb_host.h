#pragma once

#include <stddef.h>
#include <stdint.h>

#include <mios/device.h>

#include "usb_desc.h"

// Values match the port speed field of the DWC2 HPRT register
#define USB_SPEED_HIGH 0
#define USB_SPEED_FULL 1
#define USB_SPEED_LOW  2

#define USB_HOST_MAX_BINDINGS 8

typedef struct usb_host_ctrl usb_host_ctrl_t;
typedef struct usb_host_device usb_host_device_t;

// A pipe is an open bulk endpoint on a device. Opaque, owned by the
// host controller driver
typedef struct usb_host_pipe usb_host_pipe_t;

typedef struct usb_host_xfer {
  // DMA reachable buffer. For IN transfers it must be cache line aligned
  // and sized, and ux_len should be a multiple of the max packet size
  void *ux_buf;
  size_t ux_len;

  // Number of bytes transferred, or an error
  int ux_result;

  // Called from interrupt context (IRQ_LEVEL_IO) when the transfer is
  // done. The transfer may be resubmitted from the callback
  void (*ux_done)(struct usb_host_xfer *ux);
  void *ux_opaque;
} usb_host_xfer_t;


typedef struct usb_host_ctrl_vtable {

  // Blocking control transfer (SETUP, optional data stage, status).
  // setup->length bytes are read from or written to data depending on
  // the direction bit in setup->request_type.
  // Returns number of bytes transferred in the data stage or an error
  int (*control)(usb_host_ctrl_t *uhc, uint8_t addr, uint8_t speed,
                 uint16_t mps0, const struct usb_setup_packet *setup,
                 void *data);

  usb_host_pipe_t *(*pipe_open)(usb_host_ctrl_t *uhc, uint8_t addr,
                                uint8_t speed,
                                const struct usb_endpoint_descriptor *ed);

  // Aborts a pending transfer (it completes with ERR_INTERRUPTED before
  // this returns) and frees the pipe. Thread context only
  void (*pipe_close)(usb_host_ctrl_t *uhc, usb_host_pipe_t *up);

  // One transfer at a time per pipe. Callable from interrupt context
  error_t (*submit)(usb_host_ctrl_t *uhc, usb_host_pipe_t *up,
                    usb_host_xfer_t *ux);

} usb_host_ctrl_vtable_t;


struct usb_host_ctrl {
  device_t uhc_dev;
  const usb_host_ctrl_vtable_t *uhc_vtable;
  usb_host_device_t *uhc_root; // Device attached to the root port
};


typedef struct usb_host_driver {

  // Called from the host thread for each interface (alternate setting 0)
  // of a newly configured device that no driver has claimed yet. The
  // interface descriptor points into ud_config, so the class specific
  // and endpoint descriptors that follow can be parsed.
  // Return an instance pointer to claim the interface, NULL otherwise.
  // A driver that takes more interfaces sets their bits in ud_claimed
  void *(*uhd_probe)(usb_host_device_t *ud,
                     const struct usb_interface_descriptor *id);

  // The device is gone. Close all pipes and drop references to ud
  void (*uhd_detach)(usb_host_device_t *ud, void *instance);

  struct usb_host_driver *uhd_next;

} usb_host_driver_t;


struct usb_host_device {
  device_t ud_dev;
  usb_host_ctrl_t *ud_uhc;

  uint8_t ud_addr;
  uint8_t ud_speed;
  uint16_t ud_mps0;

  struct usb_device_descriptor ud_desc;

  uint8_t *ud_config;  // Full configuration descriptor
  size_t ud_config_len;

  char *ud_manufacturer;
  char *ud_product;
  char *ud_serial;

  uint32_t ud_claimed; // Bitmask of interface numbers bound to drivers

  struct {
    usb_host_driver_t *driver;
    void *instance;
  } ud_bindings[USB_HOST_MAX_BINDINGS];

  char ud_name[12];
};


int usb_host_control(usb_host_device_t *ud, uint8_t request_type,
                     uint8_t request, uint16_t value, uint16_t index,
                     void *data, uint16_t length);

usb_host_pipe_t *usb_host_pipe_open(usb_host_device_t *ud,
                                    const struct usb_endpoint_descriptor *ed);

void usb_host_pipe_close(usb_host_device_t *ud, usb_host_pipe_t *up);

error_t usb_host_submit(usb_host_device_t *ud, usb_host_pipe_t *up,
                        usb_host_xfer_t *ux);

// Descriptor helpers, all searching within ud_config

const struct usb_interface_descriptor *
usb_host_find_interface(usb_host_device_t *ud, int ifnum);

// Next descriptor of the given type following 'from' but before the
// next interface descriptor. NULL if there is none
const void *usb_host_find_desc(usb_host_device_t *ud, const void *from,
                               int type);

// First endpoint of the interface with matching transfer type and
// direction (0x80 for IN, 0 for OUT)
const struct usb_endpoint_descriptor *
usb_host_find_endpoint(usb_host_device_t *ud,
                       const struct usb_interface_descriptor *id,
                       int type, int dir);

// Class drivers are registered by board code so that unused drivers
// are not linked in
void usb_host_driver_register(usb_host_driver_t *uhd);

// Called by host controller drivers from thread context once a device
// on the root port has been reset (attach) or has gone away (detach)

void usb_host_attach(usb_host_ctrl_t *uhc, int speed);

void usb_host_detach(usb_host_ctrl_t *uhc);


// CDC-ACM class driver

void usb_host_acm_register(void);

// Returns a stream for the nth attached CDC-ACM device, or NULL.
// Reads and writes fail with ERR_NOT_CONNECTED once the device is gone.
// Close the stream when done
struct stream *usb_host_acm_open(int unit);
