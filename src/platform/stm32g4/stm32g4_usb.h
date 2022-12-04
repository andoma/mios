#pragma once

#include <usb/usb.h>
#include <mios/io.h>

void stm32g4_usb_create(uint16_t vid, uint16_t pid,
                        const char *manfacturer_string,
                        const char *product_string,
                        struct usb_interface_queue *q);

void stm32g4_usb_disable(int yes);

void stm32g4_usb_stop(void);
