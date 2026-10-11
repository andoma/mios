#pragma once

#include <stdint.h>

#include "error.h"

struct stream;

// Raw Bayer image sensor connected over MIPI CSI-2

#define IMAGE_SENSOR_BAYER_RGGB 0
#define IMAGE_SENSOR_BAYER_GRBG 1
#define IMAGE_SENSOR_BAYER_GBRG 2
#define IMAGE_SENSOR_BAYER_BGGR 3

typedef struct image_sensor {

  const char *name;

  // Output format
  uint16_t width;
  uint16_t height;
  uint8_t fps;
  uint8_t bayer;            // IMAGE_SENSOR_BAYER_xxx
  uint8_t bits;             // Bits per component
  uint16_t black_level;     // At `bits` scale

  // CSI-2 link
  uint8_t csi_lanes;
  uint8_t csi_data_type;    // E.g. 0x2b for RAW10
  uint16_t csi_lane_mbps;

  // Exposure is in lines, gain is linear with 256 = 1.0
  uint32_t max_exposure;
  uint32_t line_time_ns;
  uint32_t max_gain;

  error_t (*start)(struct image_sensor *is);

  error_t (*stop)(struct image_sensor *is);

  // These take effect a few frames later (sensor dependent)
  error_t (*set_exposure)(struct image_sensor *is, uint32_t lines);

  error_t (*set_gain)(struct image_sensor *is, uint32_t gain);

  // Sensor specific test pattern number, or -1 for normal operation
  error_t (*set_test_pattern)(struct image_sensor *is, int pattern);

  void (*print_info)(struct image_sensor *is, struct stream *st);

} image_sensor_t;
