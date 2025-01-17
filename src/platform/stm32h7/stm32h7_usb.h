#pragma once

#include <stdint.h>

struct usb_interface_queue;
void stm32h7_otghs_create(uint16_t vid, uint16_t pid,
                          const char *manfacturer_string,
                          const char *product_string,
                          struct usb_interface_queue *q);
