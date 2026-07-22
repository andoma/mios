#pragma once

#include <stdint.h>

struct usb_interface_queue;

// Bring up the nRF52840 USBD peripheral as a USB 2.0 full-speed device and
// attach the interfaces queued in q (CDC/DFU/etc). Call from board init after
// the interface classes have populated q.
void nrf52840_usb_create(uint16_t vid, uint16_t pid,
                         const char *manufacturer, const char *product,
                         struct usb_interface_queue *q);
