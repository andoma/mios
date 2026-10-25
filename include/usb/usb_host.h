#pragma once

#include <stddef.h>
#include <stdint.h>

#include <mios/device.h>

#include "usb_desc.h"

// Values match the port speed field of the DWC2 HPRT register
#define USB_SPEED_HIGH 0
#define USB_SPEED_FULL 1
#define USB_SPEED_LOW  2

typedef struct usb_host_ctrl usb_host_ctrl_t;

typedef struct usb_host_ctrl_vtable {

  // Blocking control transfer (SETUP, optional data stage, status).
  // setup->length bytes are read from or written to data depending on
  // the direction bit in setup->request_type.
  // Returns number of bytes transferred in the data stage or an error
  int (*control)(usb_host_ctrl_t *uhc, uint8_t addr, uint8_t speed,
                 uint16_t mps0, const struct usb_setup_packet *setup,
                 void *data);

} usb_host_ctrl_vtable_t;


struct usb_host_ctrl {
  device_t uhc_dev;
  const usb_host_ctrl_vtable_t *uhc_vtable;
  struct usb_host_device *uhc_root; // Device attached to the root port
};


typedef struct usb_host_device {
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

  char ud_name[12];
} usb_host_device_t;


int usb_host_control(usb_host_device_t *ud, uint8_t request_type,
                     uint8_t request, uint16_t value, uint16_t index,
                     void *data, uint16_t length);

// Called by host controller drivers from thread context once a device
// on the root port has been reset (attach) or has gone away (detach)

void usb_host_attach(usb_host_ctrl_t *uhc, int speed);

void usb_host_detach(usb_host_ctrl_t *uhc);
