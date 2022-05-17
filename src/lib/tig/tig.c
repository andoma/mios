#include "tig.h"

#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <stdio.h>

struct tig_ctx {

  gfx_position_t tc_pos;

  struct gfx_display *tc_gd;

  void (*tc_draw)(tig_ctx_t *ctx);

  gfx_font_id_t tc_font;
};


void *
tig_make_delegate(void (*draw)(tig_ctx_t *ctx))
{
  tig_ctx_t *tc = calloc(1, sizeof(tig_ctx_t));
  tc->tc_draw = draw;
  return tc;
}

/***********************************************************************
 *
 * GFX delegates
 *
 */

static void
tig_draw_display(void *opaque, struct gfx_display *gd,
                 const gfx_rect_t *display_size)
{
  tig_ctx_t *tc = opaque;
  tc->tc_gd = gd;

  tc->tc_font = 5;
  tc->tc_pos.x = 0;
  tc->tc_pos.y = 0;
  tc->tc_draw(tc);
}


const gfx_display_delegate_t tig_display_delegate = {
  .gdd_draw = tig_draw_display,
};



void
tig_move_abs(tig_ctx_t *tc, int x, int y)
{
  tc->tc_pos.x = x;
  tc->tc_pos.y = y;
}

void
tig_move_rel(tig_ctx_t *tc, int x, int y)
{
  tc->tc_pos.x += x;
  tc->tc_pos.y += y;
}


void
tig_text(tig_ctx_t *tc, const char *fmt, ...)
{
  gfx_display_t *gd = tc->tc_gd;
  const gfx_display_class_t *gdc = gd->gd_class;

  char tmp[64];
  va_list ap;
  va_start(ap, fmt);
  size_t len = vsnprintf(tmp, sizeof(tmp), fmt, ap);
  va_end(ap);

  gfx_size_t size = gdc->get_text_size(gd, tc->tc_font, tmp, len);
  gdc->text(gd, &tc->tc_pos, tc->tc_font, tmp, len);
  tc->tc_pos.y += size.height;
}

