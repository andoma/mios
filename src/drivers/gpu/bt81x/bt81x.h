#pragma once

#include <mios/io.h>
#include <mios/gfx.h>

typedef struct {
  const void *data;
  size_t size;
} bt81x_bitmap_t;

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

  // clock multiplier for external crystal.
  // if 0 == use internal clock

  uint8_t clksel;

  uint8_t pclk; // pixel clock divider

} bt81x_timings_t;

__attribute__((access(read_only, 6), access(read_only, 7, 8)))
struct gfx_display *bt81x_create(spi_t *spi, gpio_t ncs, gpio_t pd, gpio_t irq,
                                 gpio_output_speed_t drive_strength,
                                 const bt81x_timings_t *timings,
                                 const bt81x_bitmap_t bitmaps[],
                                 size_t num_bitmaps,
                                 const gfx_display_delegate_t *gdd,
                                 void *opaque,
                                 int enabled);

void bt81x_enable(struct gfx_display *, int enabled);

void bt81x_backlight(struct gfx_display *, uint8_t backlight);
