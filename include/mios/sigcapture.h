#pragma once

#include <stddef.h>
#include <stdint.h>

struct usb_interface_queue;

typedef struct sigcapture sigcapture_t;

typedef enum {
  SIGCAPTURE_UNIT_UNUSED,
  SIGCAPTURE_UNIT_1,
  SIGCAPTURE_UNIT_VOLTAGE,
  SIGCAPTURE_UNIT_CURRENT,
  SIGCAPTURE_UNIT_TEMPERATURE,
} sigcapture_unit_t;

typedef struct sigcapture_desc {
  const char *name;
  float scale;
  sigcapture_unit_t unit;
} sigcapture_desc_t;

sigcapture_t *sigcapture_create(size_t depth_power_of_2, size_t channels,
                                const sigcapture_desc_t channel_descriptors[],
                                uint32_t nominal_frequency,
                                struct usb_interface_queue *q,
                                uint8_t usb_iface_subtype);

int16_t *sigcapture_wrptr(sigcapture_t *sc);

void sigcapture_trig(sigcapture_t *sc, size_t leading_samples);
