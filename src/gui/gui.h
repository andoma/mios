#pragma once

#include <sys/queue.h>
#include <stdint.h>
#include <pthread.h>

STAILQ_HEAD(gui_widget_queue, gui_widget);

typedef struct gui_display gui_display_t;
typedef struct gui_widget gui_widget_t;

typedef int16_t gui_scalar_t;
typedef uint8_t gui_font_id_t;

typedef struct {
  gui_scalar_t x;
  gui_scalar_t y;
} gui_position_t;

typedef struct {
  gui_scalar_t width;
  gui_scalar_t height;
} gui_size_t;

typedef struct {
  gui_position_t pos;
  gui_size_t siz;
} gui_rect_t;


#define GUI_DISPLAY_PALETTE_SIZE 4
#define GUI_COLOR_PALETTE(x) (0x80000000 | (x))

typedef struct gui_display_class {

  void (*push_state)(gui_display_t *gd);

  void (*pop_state)(gui_display_t *gd);

  void (*scissor)(gui_display_t *gd, const gui_rect_t *r);

  void (*line)(gui_display_t *gd,
               int x1, int y1, int x2, int y2,
               int line_width);

  void (*rect)(gui_display_t *gd,
               const gui_rect_t *r,
               int line_width);

  void (*filled_rect)(gui_display_t *gd,
                      const gui_rect_t *r,
                      int corner_radius);

  void (*text)(gui_display_t *gd,
               const gui_position_t *pos,
               gui_font_id_t font,
               const char *text,
               size_t len);

  void (*bitmap)(gui_display_t *gd,
                 const gui_position_t *pos,
                 int bitmap_id);

  void (*set_color)(gui_display_t *gd, uint32_t rgb);

  void (*set_alpha)(gui_display_t *gd, uint8_t alpha);

  gui_size_t (*get_text_size)(gui_display_t *gd,
                              gui_font_id_t font,
                              const char *text,
                              size_t len);

  int (*get_font_baseline)(gui_display_t *gd, gui_font_id_t font);

  gui_size_t (*get_bitmap_size)(gui_display_t *gd, int bitmap);

} gui_display_class_t;



struct gui_display {
  const gui_display_class_t *gd_class;
  gui_widget_t *gd_root;
  gui_widget_t *gd_grab;
  pthread_mutex_t gd_mutex;

  // Color 0 is always clear color
  // Color 1 is default text color
  uint32_t gd_palette[GUI_DISPLAY_PALETTE_SIZE];
};

void gui_display_init(gui_display_t *gd);


typedef struct gui_widget_class {

  size_t instance_size;

  void (*update_req)(gui_widget_t *w, gui_display_t *gd);

  void (*layout)(gui_widget_t *w, gui_display_t *gd);

  void (*draw)(gui_widget_t *w, gui_display_t *gd);

  void (*add_child)(gui_widget_t *w, gui_widget_t *c);

  gui_widget_t *(*grab)(gui_widget_t *w, gui_display_t *gd,
                        const gui_position_t *p,
                        int descend);

  void (*move)(gui_widget_t *w, gui_display_t *gd, const gui_position_t *p);

  void (*release)(gui_widget_t *w, gui_display_t *gd);

} gui_widget_class_t;


struct gui_widget {
  STAILQ_ENTRY(gui_widget) gw_parent_link;
  struct gui_widget *gw_parent;

  const gui_widget_class_t *gw_class;

  gui_rect_t gw_rect;

  gui_size_t gw_req_size;

  uint16_t gw_flags;
#define GUI_WIDGET_NEED_LAYOUT       0x1
#define GUI_WIDGET_NEED_UPDATE_REQ   0x2
#define GUI_WIDGET_CONSTRAIN_X       0x4
#define GUI_WIDGET_CONSTRAIN_Y       0x8
#define GUI_WIDGET_HIDDEN_BY_PARENT  0x10 // Hidden by parent (out of frame)
#define GUI_WIDGET_BASELINE          0x20

#define GUI_WIDGET_DEBUG             0x8000

  // Numpad style alignment
#define GW_ALIGN_NONE          0
#define GW_ALIGN_BOTTOM_LEFT   1
#define GW_ALIGN_BOTTOM_CENTER 2
#define GW_ALIGN_BOTTOM_RIGHT  3
#define GW_ALIGN_MID_LEFT      4
#define GW_ALIGN_MID_CENTER    5
#define GW_ALIGN_MID_RIGHT     6
#define GW_ALIGN_TOP_LEFT      7
#define GW_ALIGN_TOP_CENTER    8
#define GW_ALIGN_TOP_RIGHT     9

  uint8_t gw_alignment;
  int8_t gw_margin_top;
  int8_t gw_margin_right;
  int8_t gw_margin_bottom;
  int8_t gw_margin_left;

  int8_t gw_weight;
  uint8_t gw_baseline;
};


typedef struct gui_container {
  gui_widget_t gw;
  struct gui_widget_queue gc_children;
} gui_container_t;




void gui_set_alignment(gui_widget_t *gw, uint8_t alignment);

void gui_set_margin_all(gui_widget_t *gw, int8_t margin);

void gui_set_margin(gui_widget_t *gw,
                    int8_t top, int8_t right,
                    int8_t bottom, int8_t left);

void gui_set_color(gui_widget_t *w, uint32_t color);

void gui_attrib_changed(gui_widget_t *gw);

void gui_draw_display(gui_display_t *gd, const gui_rect_t *r);

void gui_touch_release(gui_display_t *gd);

void gui_touch_press(gui_display_t *gd, const gui_position_t *p);

// ==========================================================

void *gui_create_from_classdef(gui_widget_t *p,
                               const gui_widget_class_t *gwc);

gui_widget_t *gui_create_list(gui_widget_t *p);

gui_widget_t *gui_create_vbox(gui_widget_t *p);

gui_widget_t *gui_create_hbox(gui_widget_t *p);

gui_widget_t *gui_create_zbox(gui_widget_t *p);

gui_widget_t *gui_create_abox(gui_widget_t *p);

gui_widget_t *gui_create_quad(gui_widget_t *p,
                              uint32_t background_color,
                              uint32_t border_color,
                              uint32_t border_linesize);

gui_widget_t *gui_create_cstr(gui_widget_t *p, const char *str,
                              gui_font_id_t font_id, uint8_t alignment);

gui_widget_t *gui_create_dstr(gui_widget_t *p, size_t maxlen,
                              gui_font_id_t font_id, uint8_t alignment);

void gui_set_dstr(gui_widget_t *w, const char *str);

gui_widget_t *gui_create_vsep(gui_widget_t *p, int thickness, int margin);

gui_widget_t *gui_create_hsep(gui_widget_t *p, int thickness, int margin);

gui_widget_t *gui_create_bitmap(gui_widget_t *p, int bitmap);
