#pragma once

#include <mios/io.h>



typedef struct {

  // Horizontal
  uint16_t width;
  uint16_t hsync0;
  uint16_t hsync1;
  uint16_t hoffset;
  uint16_t hcycle;

  // Vertical
  uint16_t height;
  uint16_t vsync0;
  uint16_t vsync1;
  uint16_t voffset;
  uint16_t vcycle;

  // Swizzle
  uint8_t swizzle;

  // Pixelclock polarity
  uint8_t pclk_pol;

} bt81x_timings_t;

struct gui_display;

struct gui_display *bt81x_create(spi_t *spi, gpio_t ncs, gpio_t pd, gpio_t irq,
                                 const bt81x_timings_t *timings);
