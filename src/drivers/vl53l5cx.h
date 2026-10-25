#pragma once

#include <stdint.h>
#include <mios/error.h>

struct i2c;
struct stream;

// ST VL53L5CX multizone time-of-flight ranging sensor: 4x4 or 8x8 zones,
// 63 degree diagonal field of view, up to 4 m. The sensor has no flash,
// its firmware (84 KB) is uploaded over I2C by vl53l5cx_create()

#define VL53L5CX_I2C_ADDR 0x29

#define VL53L5CX_MAX_ZONES 64

typedef struct vl53l5cx vl53l5cx_t;

typedef struct {
  uint8_t zones;                         // 16 or 64
  uint8_t stream_count;                  // Increments for every frame
  int16_t distance_mm[VL53L5CX_MAX_ZONES];
  uint16_t sigma_mm[VL53L5CX_MAX_ZONES];   // Range noise estimate
  uint32_t signal[VL53L5CX_MAX_ZONES];     // kcps per SPAD
  uint32_t ambient[VL53L5CX_MAX_ZONES];    // kcps per SPAD
  uint8_t status[VL53L5CX_MAX_ZONES];      // 5 = valid, 6, 9 = 50% valid,
                                           // 255 = no target
} vl53l5cx_frame_t;

// Zones are in row-major order as seen from the sensor (ST's UM2884
// describes the zone map relative to the package orientation).

// Probe the sensor, upload the firmware and the default configuration.
// Takes a few seconds at 400 kHz. The sensor must be powered with LPn high.
vl53l5cx_t *vl53l5cx_create(struct i2c *bus);

// zones_per_side: 4 or 8. hz: 1-60 for 4x4, 1-15 for 8x8
error_t vl53l5cx_start(vl53l5cx_t *v, unsigned int zones_per_side,
                       unsigned int hz);

error_t vl53l5cx_stop(vl53l5cx_t *v);

int vl53l5cx_is_ranging(vl53l5cx_t *v);

// Wait for the next frame (polls the sensor)
error_t vl53l5cx_read(vl53l5cx_t *v, vl53l5cx_frame_t *f, int64_t deadline);

static inline int
vl53l5cx_zone_valid(const vl53l5cx_frame_t *f, unsigned int zone)
{
  return f->status[zone] == 5 || f->status[zone] == 6 || f->status[zone] == 9;
}

void vl53l5cx_print_frame(const vl53l5cx_frame_t *f, struct stream *st);
