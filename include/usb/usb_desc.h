#pragma once

#include <stdint.h>

#define USB_DESC_TYPE_DEVICE            1
#define USB_DESC_TYPE_CONFIGURATION     2
#define USB_DESC_TYPE_STRING            3
#define USB_DESC_TYPE_INTERFACE         4
#define USB_DESC_TYPE_ENDPOINT          5
#define USB_DESC_TYPE_QUALIFIER         6
#define USB_DESC_TYPE_DEBUG             10
#define USB_DESC_TYPE_INTERFACE_ASSOC   11
#define USB_DESC_TYPE_CS_INTERFACE      36

#define USB_ENDPOINT_CONTROL            0
#define USB_ENDPOINT_ISOCHRONUS         1
#define USB_ENDPOINT_BULK               2
#define USB_ENDPOINT_INTERRUPT          3

#define USB_REQ_GET_STATUS              0
#define USB_REQ_SET_ADDRESS             5
#define USB_REQ_GET_DESCRIPTOR          6
#define USB_REQ_SET_CONFIG              9

struct usb_device_descriptor {
  uint8_t  bLength;
  uint8_t  bDescriptorType;
  uint16_t bcdUSB;
  uint8_t  bDeviceClass;
  uint8_t  bDeviceSubClass;
  uint8_t  bDeviceProtocol;
  uint8_t  bMaxPacketSize0;
  uint16_t idVendor;
  uint16_t idProduct;
  uint16_t bcdDevice;
  uint8_t  iManufacturer;
  uint8_t  iProduct;
  uint8_t  iSerialNumber;
  uint8_t  bNumConfigurations;
} __attribute__((packed));


struct usb_config_descriptor {
  uint8_t  bLength;
  uint8_t  bDescriptorType;
  uint16_t wTotalLength;
  uint8_t  bNumInterfaces;
  uint8_t  bConfigurationValue;
  uint8_t  iConfiguration;
  uint8_t  bmAttributes;
  uint8_t  bMaxPower;
} __attribute__((packed));


struct usb_iad_descriptor {
  uint8_t bLength;
  uint8_t bDescriptorType;
  uint8_t bFirstInterface;
  uint8_t bInterfaceCount;
  uint8_t bFunctionClass;
  uint8_t bFunctionSubClass;
  uint8_t bFunctionProtocol;
  uint8_t iFunction;
} __attribute__((packed));


struct usb_interface_descriptor {
  uint8_t bLength;
  uint8_t bDescriptorType;
  uint8_t bInterfaceNumber;
  uint8_t bAlternateSetting;
  uint8_t bNumEndpoints;
  uint8_t bInterfaceClass;
  uint8_t bInterfaceSubClass;
  uint8_t bInterfaceProtocol;
  uint8_t iInterface;
} __attribute__((packed));


struct usb_endpoint_descriptor {
  uint8_t  bLength;
  uint8_t  bDescriptorType;
  uint8_t  bEndpointAddress;
  uint8_t  bmAttributes;
  uint16_t wMaxPacketSize;
  uint8_t  bInterval;
} __attribute__((packed));
