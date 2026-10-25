#pragma once

#include <stdint.h>
#include <mios/io.h>

struct usb_interface_queue;

void stm32n6_otghs_create(uint16_t vid, uint16_t pid,
                          const char *manfacturer_string,
                          const char *product_string,
                          struct usb_interface_queue *q);

// OTG2 in host mode. vbus_enable is driven high to power the port
void stm32n6_otghs2_host_create(gpio_t vbus_enable);
