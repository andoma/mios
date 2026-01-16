#pragma once

#include <pthread.h>
#include <stdint.h>

typedef struct gfx_display gfx_display_t;

typedef int16_t gfx_scalar_t;
typedef uint8_t gfx_font_id_t;

typedef struct {
  gfx_scalar_t x;
  gfx_scalar_t y;
} gfx_position_t;

typedef struct {
  gfx_scalar_t width;
  gfx_scalar_t height;
} gfx_size_t;

typedef struct {
  gfx_position_t pos;
  gfx_size_t siz;
} gfx_rect_t;

typedef enum {
  GFX_PRIMITIVE_LINES,
  GFX_PRIMITIVE_RECTANGLES,
  GFX_PRIMITIVE_POINTS,
} gfx_primitive_t;

#define GFX_DISPLAY_PALETTE_SIZE 16
#define GFX_COLOR_PALETTE(x) (0x80000000 | (x))

typedef struct gfx_display_class {

  void (*push_state)(gfx_display_t *gd);

  void (*pop_state)(gfx_display_t *gd);

  void (*scissor)(gfx_display_t *gd, const gfx_rect_t *r);

  void (*line)(gfx_display_t *gd,
               int x1, int y1, int x2, int y2,
               int line_width);

  void (*rect)(gfx_display_t *gd,
               const gfx_rect_t *r,
               int line_width);

  void (*filled_rect)(gfx_display_t *gd,
                      const gfx_rect_t *r,
                      int corner_radius);

  int (*text)(gfx_display_t *gd,
              const gfx_position_t *pos,
               gfx_font_id_t font,
              const char *text,
              size_t len);

  void (*bitmap)(gfx_display_t *gd,
                 const gfx_position_t *pos,
                 int bitmap_id);

  void (*set_color)(gfx_display_t *gd, uint32_t rgb);

  void (*set_alpha)(gfx_display_t *gd, uint8_t alpha);

  gfx_size_t (*get_text_size)(gfx_display_t *gd,
                              gfx_font_id_t font,
                              const char *text,
                              size_t len);

  int (*get_font_baseline)(gfx_display_t *gd, gfx_font_id_t font);

  int (*get_font_height)(gfx_display_t *gd, gfx_font_id_t font);

  gfx_size_t (*get_bitmap_size)(gfx_display_t *gd, int bitmap);

  void (*begin)(gfx_display_t *gd, gfx_primitive_t primitive, float attribute);

  void (*vertex)(gfx_display_t *gd, float x, float y);

  void (*end)(gfx_display_t *gd);


} gfx_display_class_t;



struct gfx_display;

typedef struct gfx_display_delegate {

  void (*gdd_prep)(void *opaque, struct gfx_display *gd,
                   const gfx_rect_t *display_size);

  void (*gdd_draw)(void *opaque, struct gfx_display *gd,
                   const gfx_rect_t *display_size);

  void (*gdd_touch_release)(void *opaque, struct gfx_display *gd);

  void (*gdd_touch_press)(void *opaque, struct gfx_display *gd,
                          const gfx_position_t *p);

} gfx_display_delegate_t;


struct gfx_display {
  const gfx_display_class_t *gd_class;

  const gfx_display_delegate_t *gd_gdd;
  void *gd_opaque;

  pthread_mutex_t gd_mutex;

  // Color 0 is always clear color
  // Color 1 is default text color
  uint32_t gd_palette[GFX_DISPLAY_PALETTE_SIZE];

  // Controlls where \t jumps in text
  const int *gd_tab_offsets;
  size_t gd_tab_offsets_size;
};
