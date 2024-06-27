#pragma once

#include <mios/gfx.h>

typedef enum {
  TTS_NONE,
  TTS_PRESS,
  TTS_RELEASE,
} tig_tap_state_t;

// TIG - Tiny Immediate-mode Gui

typedef struct tig_ctx tig_ctx_t;

void *tig_make_delegate(void (*draw)(tig_ctx_t *ctx, void *opaque),
                        void (*prep)(tig_ctx_t *ctx, void *opaque),
                        void *opaque);

extern const gfx_display_delegate_t tig_display_delegate;

// State methods

void tig_push(tig_ctx_t *ctx);

void tig_pop(tig_ctx_t *ctx);

void tig_view(tig_ctx_t *tc, int width, int height);

void tig_move_abs(tig_ctx_t *tc, int x, int y);

void tig_move_rel(tig_ctx_t *tc, int x, int y);

void tig_set_color(tig_ctx_t *tc, uint32_t color);

void tig_set_alpha(tig_ctx_t *tc, uint8_t alpha);

void tig_set_font(tig_ctx_t *tc, int id);

void tig_set_tab_offsets(tig_ctx_t *tc, const int offsets[],
                         size_t length);

void tig_indent(tig_ctx_t *tc);

void tig_unindent(tig_ctx_t *tc);

int tig_button(tig_ctx_t *tc, int flags, int width, const char *str);

void tig_nextline(tig_ctx_t *tc, int height);

// Interactions

tig_tap_state_t tig_tap(tig_ctx_t *tc);

typedef struct {
  float position;
  float origin;
  float velocity;
  int height;
  uint8_t grab;
} tig_scroll_state_t;

void tig_scroll_begin(tig_ctx_t *tc, tig_scroll_state_t *tss);

void tig_scroll_end(tig_ctx_t *tc, tig_scroll_state_t *tss);

// Draw method

#define TIG_HALIGN_RIGHT 0x1
#define TIG_HALIGN_VIEW_CENTERED 0x2

#define TIG_VALIGN_TOP 0x4
#define TIG_VALIGN_VIEW_CENTERED 0x8

#define TIG_NO_INDENT 0x10

#define TIG_VIEW_CENTERED (TIG_HALIGN_VIEW_CENTERED | \
                           TIG_VALIGN_VIEW_CENTERED)

#define TIG_INLINE 0x20

void tig_text(tig_ctx_t *ctx, int flags, const char *fmt, ...)
  __attribute__ ((format(printf, 3, 4)));


void tig_bitmap(tig_ctx_t *tc, int id);

void tig_hsep(tig_ctx_t *tc, uint8_t alpha);

void tig_fill(tig_ctx_t *tc);

void tig_rect(tig_ctx_t *tc);


// Utility methods

uint32_t tig_hsv_to_rgb(int H, uint8_t S, uint8_t V);

struct gfx_display *tig_get_gd(tig_ctx_t *tc);
