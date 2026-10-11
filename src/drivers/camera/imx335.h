#pragma once

#include <mios/image_sensor.h>

struct i2c;

// Sony IMX335 5 Mpixel image sensor: 2592x1944 RAW10 (RGGB) over two
// CSI-2 data lanes, INCK = 24 MHz

#define IMX335_I2C_ADDR 0x1a

// Probe and configure the sensor (left in standby). The sensor must be
// powered and out of reset.
image_sensor_t *imx335_create(struct i2c *bus, int fps);
