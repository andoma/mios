#pragma once

#include <usb/usb.h>

void stm32f4_otgfs_create(uint16_t vid, uint16_t pid,
                          const char *manfacturer_string,
                          const char *product_string,
                          struct usb_interface_queue *q);
