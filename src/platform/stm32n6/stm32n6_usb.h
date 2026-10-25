#pragma once

#include <stdint.h>
#include <mios/io.h>

struct usb_interface_queue;

// OTG1 in device mode, starts right away
void stm32n6_otghs_create(uint16_t vid, uint16_t pid,
                          const char *manfacturer_string,
                          const char *product_string,
                          struct usb_interface_queue *q);

// Turn OTG1 device mode off and on, for dual role ports. While off the
// core can be used in host mode
void stm32n6_otghs_device_enable(int on);

// OTG2 in host mode. vbus_enable is driven high to power the port
void stm32n6_otghs2_host_create(gpio_t vbus_enable);

// OTG1 in host mode for dual role ports, starts out disabled. VBUS is up
// to the caller. Only one of host and device mode may be enabled
void stm32n6_otghs1_host_create(void);

void stm32n6_otghs1_host_enable(int on);

// OTG1 clocks and PHY, done once. Used by both device and host mode
void stm32n6_otghs1_clk_init(void);
