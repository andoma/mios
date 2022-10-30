#include "tig.h"

#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <stdio.h>

typedef struct tig_state {
  gfx_position_t ts_pos;
  gfx_font_id_t ts_font;
  gfx_rect_t ts_view;

  uint8_t ts_lazy_gfx_push;

  uint8_t ts_indent;

} tig_state_t;

#define TIG_STATE_DEPTH 10

struct tig_ctx {

  struct gfx_display *tc_gd;

  void (*tc_draw)(tig_ctx_t *ctx);

  gfx_position_t tc_touch_pos;

  tig_tap_state_t tc_tts;

  int tc_sptr;

  tig_state_t tc_state[TIG_STATE_DEPTH];
};


void *
tig_make_delegate(void (*draw)(tig_ctx_t *ctx))
{
  tig_ctx_t *tc = calloc(1, sizeof(tig_ctx_t));
  tc->tc_draw = draw;
  return tc;
}


static void
tig_lazy_gfx_push(tig_ctx_t *tc)
{
  if(tc->tc_sptr == 0)
    return;

  tig_state_t *ts = &tc->tc_state[tc->tc_sptr];
  if(ts->ts_lazy_gfx_push)
    return;

  gfx_display_t *gd = tc->tc_gd;
  const gfx_display_class_t *gdc = gd->gd_class;
  gdc->push_state(gd);

  ts->ts_lazy_gfx_push = 1;
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
  tc->tc_sptr = 0;
  tig_state_t *ts = &tc->tc_state[tc->tc_sptr];
  ts->ts_view = *display_size;
  ts->ts_font = 5;
  ts->ts_pos.x = 0;
  ts->ts_pos.y = 0;
  ts->ts_lazy_gfx_push = 0;
  tc->tc_draw(tc);

  if(tc->tc_tts == TTS_RELEASE)
    tc->tc_tts = TTS_NONE;
}


static void
tig_touch_release(void *opaque, struct gfx_display *gd)
{
  tig_ctx_t *tc = opaque;
  if(tc->tc_tts == TTS_PRESS)
    tc->tc_tts = TTS_RELEASE;
}

static void
tig_touch_press(void *opaque, struct gfx_display *gd,
                const gfx_position_t *p)
{
  tig_ctx_t *tc = opaque;
  tc->tc_touch_pos = *p;
  tc->tc_tts = TTS_PRESS;
}

const gfx_display_delegate_t tig_display_delegate = {
  .gdd_draw = tig_draw_display,
  .gdd_touch_release = tig_touch_release,
  .gdd_touch_press = tig_touch_press,
};

/***********************************************************************
 *
 * API
 *
 */

struct gfx_display *
tig_get_gd(tig_ctx_t *tc)
{
  return tc->tc_gd;
}


void tig_push(tig_ctx_t *tc)
{
  if(tc->tc_sptr == TIG_STATE_DEPTH - 1)
    return;

  tc->tc_sptr++;
  tc->tc_state[tc->tc_sptr] = tc->tc_state[tc->tc_sptr - 1];
  tc->tc_state[tc->tc_sptr].ts_lazy_gfx_push = 0;
}

void tig_pop(tig_ctx_t *tc)
{
  if(tc->tc_sptr == 0) {
    return;
  }

  tig_state_t *ts = &tc->tc_state[tc->tc_sptr];
  if(ts->ts_lazy_gfx_push) {
    gfx_display_t *gd = tc->tc_gd;
    const gfx_display_class_t *gdc = gd->gd_class;
    gdc->pop_state(gd);
  }
  tc->tc_sptr--;
}

void
tig_indent(tig_ctx_t *tc)
{
  tig_state_t *ts = &tc->tc_state[tc->tc_sptr];
  ts->ts_indent++;
}


void
tig_unindent(tig_ctx_t *tc)
{
  tig_state_t *ts = &tc->tc_state[tc->tc_sptr];
  if(ts->ts_indent)
    ts->ts_indent--;
}


void
tig_view(tig_ctx_t *tc, int width, int height)
{
  tig_state_t *ts = &tc->tc_state[tc->tc_sptr];

  if(width < 0)
    ts->ts_view.siz.width -= (ts->ts_pos.x - ts->ts_view.pos.x) - (width + 1);
  else
    ts->ts_view.siz.width = width;

  if(height < 0)
    ts->ts_view.siz.height -= (ts->ts_pos.y - ts->ts_view.pos.y) - (height + 1);
  else
    ts->ts_view.siz.height = height;

  ts->ts_view.pos = ts->ts_pos;
}


static int
tig_pressed(tig_ctx_t *tc, const gfx_rect_t *r)
{
  const int x1 = r->pos.x;
  const int y1 = r->pos.y;
  const int x2 = x1 + r->siz.width;
  const int y2 = y1 + r->siz.height;

  return
    x1 <= tc->tc_touch_pos.x &&
    y1 <= tc->tc_touch_pos.y &&
    tc->tc_touch_pos.x < x2 &&
    tc->tc_touch_pos.y < y2;
}


tig_tap_state_t
tig_tap(tig_ctx_t *tc)
{
  const tig_state_t *ts = &tc->tc_state[tc->tc_sptr];

  if(!tig_pressed(tc, &ts->ts_view))
    return TTS_NONE;

  return tc->tc_tts;
}

void
tig_move_abs(tig_ctx_t *tc, int x, int y)
{
  tig_state_t *ts = &tc->tc_state[tc->tc_sptr];
  ts->ts_pos.x = ts->ts_view.pos.x + x;
  ts->ts_pos.y = ts->ts_view.pos.y + y;
}

void
tig_move_rel(tig_ctx_t *tc, int x, int y)
{
  tig_state_t *ts = &tc->tc_state[tc->tc_sptr];
  ts->ts_pos.x += x;
  ts->ts_pos.y += y;
}

void
tig_set_font(tig_ctx_t *tc, int id)
{
  tig_state_t *ts = &tc->tc_state[tc->tc_sptr];
  ts->ts_font = id;
}

void
tig_set_color(tig_ctx_t *tc, uint32_t color)
{
  gfx_display_t *gd = tc->tc_gd;
  const gfx_display_class_t *gdc = gd->gd_class;

  tig_lazy_gfx_push(tc);

  gdc->set_color(gd, color);
}


void
tig_set_alpha(tig_ctx_t *tc, uint8_t alpha)
{
  gfx_display_t *gd = tc->tc_gd;
  const gfx_display_class_t *gdc = gd->gd_class;

  tig_lazy_gfx_push(tc);

  gdc->set_alpha(gd, alpha);
}


void
tig_text(tig_ctx_t *tc, int flags, const char *fmt, ...)
{
  tig_state_t *ts = &tc->tc_state[tc->tc_sptr];
  gfx_position_t pos = ts->ts_pos;

  gfx_display_t *gd = tc->tc_gd;
  const gfx_display_class_t *gdc = gd->gd_class;

  char tmp[64];
  va_list ap;
  va_start(ap, fmt);
  size_t len = vsnprintf(tmp, sizeof(tmp), fmt, ap);
  va_end(ap);

  gfx_size_t size = gdc->get_text_size(gd, ts->ts_font, tmp, len);

  if(flags & TIG_HALIGN_VIEW_CENTERED) {
    pos.x += ts->ts_view.siz.width / 2 - size.width / 2;

  } else if(flags & TIG_HALIGN_RIGHT) {
    pos.x -= size.width;
  }

  if(flags & TIG_VALIGN_VIEW_CENTERED) {

    pos.y += ts->ts_view.siz.height / 2 - size.height / 2;

  } else if(!(flags & TIG_VALIGN_TOP)) {
    pos.y -= gdc->get_font_baseline(gd, ts->ts_font);
  }

  if(pos.y + size.height >= ts->ts_view.pos.y &&
     pos.y < ts->ts_view.pos.y + ts->ts_view.siz.height) {

    if(!(flags & TIG_NO_INDENT))
      pos.x += ts->ts_indent * 20;

    gdc->text(gd, &pos, ts->ts_font, tmp, len);
  }

  ts->ts_pos.y += size.height;
}


void
tig_set_tab_offsets(tig_ctx_t *tc, const int offsets[],
                    size_t length)
{
  gfx_display_t *gd = tc->tc_gd;
  gd->gd_tab_offsets = offsets;
  gd->gd_tab_offsets_size = length;
}

void
tig_bitmap(tig_ctx_t *tc, int id)
{
  tig_state_t *ts = &tc->tc_state[tc->tc_sptr];

  gfx_display_t *gd = tc->tc_gd;
  const gfx_display_class_t *gdc = gd->gd_class;

  gdc->bitmap(gd, &ts->ts_pos, id);
}


void
tig_hsep(tig_ctx_t *tc, uint8_t alpha)
{
  tig_state_t *ts = &tc->tc_state[tc->tc_sptr];

  gfx_display_t *gd = tc->tc_gd;
  const gfx_display_class_t *gdc = gd->gd_class;

  if(alpha != 255) {
    gdc->set_alpha(gd, alpha);
  }

  gdc->line(gd,
            ts->ts_view.pos.x + ts->ts_indent * 20, ts->ts_pos.y,
            ts->ts_view.pos.x + ts->ts_view.siz.width - ts->ts_indent * 40,
            ts->ts_pos.y, 1);
  ts->ts_pos.y += 1;
  if(alpha != 255) {
    gdc->set_alpha(gd, 255);
  }
}


void
tig_fill(tig_ctx_t *tc)
{
  tig_state_t *ts = &tc->tc_state[tc->tc_sptr];
  gfx_display_t *gd = tc->tc_gd;
  const gfx_display_class_t *gdc = gd->gd_class;

  gdc->filled_rect(gd, &ts->ts_view, 1);
}

void
tig_rect(tig_ctx_t *tc)
{
  tig_state_t *ts = &tc->tc_state[tc->tc_sptr];
  gfx_display_t *gd = tc->tc_gd;
  const gfx_display_class_t *gdc = gd->gd_class;

  gdc->rect(gd, &ts->ts_view, 1);
}


void
tig_scroll_begin(tig_ctx_t *tc, tig_scroll_state_t *tss)
{
  tig_state_t *ts = &tc->tc_state[tc->tc_sptr];
  gfx_display_t *gd = tc->tc_gd;
  const gfx_display_class_t *gdc = gd->gd_class;

  const int height = tss->height - ts->ts_view.siz.height;

  if(height < 0)
    return;

  tig_lazy_gfx_push(tc);
  gdc->scissor(gd, &ts->ts_view);

  float vcoeff = 0.92f;

  if(tig_tap(tc) == TTS_PRESS) {
    if(tss->grab) {
      float p = tss->origin - tc->tc_touch_pos.y;
      float v = p - tss->position;
      tss->velocity += vcoeff * (v - tss->velocity);
    } else {
      tss->grab = 1;
      tss->origin = tc->tc_touch_pos.y + tss->position;
    }
  } else {
    tss->grab = 0;
  }

  if(!tss->grab) {
    if(tss->position < 0) {
      tss->position *= 0.8f;
    } else if(tss->position > height) {
      float p = tss->position - height;
      p = p * 0.8f;
      p += height;
      tss->position = p;
    }
  }

  tss->position += tss->velocity;
  tss->velocity *= vcoeff;

  ts->ts_pos.y -= tss->position;
}

void
tig_scroll_end(tig_ctx_t *tc, tig_scroll_state_t *tss)
{
  tig_state_t *ts = &tc->tc_state[tc->tc_sptr];

  tss->height = ts->ts_pos.y + tss->position - ts->ts_view.pos.y;
}

int
tig_button(tig_ctx_t *tc, const char *str)
{
  tig_state_t *ts = &tc->tc_state[tc->tc_sptr];
  gfx_position_t pos = ts->ts_pos;

  gfx_display_t *gd = tc->tc_gd;
  const gfx_display_class_t *gdc = gd->gd_class;

  const size_t len = strlen(str);
  const gfx_size_t size = gdc->get_text_size(gd, ts->ts_font, str, len);

  pos.y -= gdc->get_font_baseline(gd, ts->ts_font);

  int hit = 0;

  if(pos.y + size.height >= ts->ts_view.pos.y &&
     pos.y < ts->ts_view.pos.y + ts->ts_view.siz.height) {

    pos.x += ts->ts_indent * 20;


    gfx_rect_t rect = {{pos.x+5, pos.y+5},
                       {ts->ts_view.siz.width - ts->ts_indent * 40 - 10,
                        20 + size.height - 10}};

    if(tig_pressed(tc, &rect)) {
      if(tc->tc_tts == TTS_RELEASE) {
        hit = 1;
      } else if(tc->tc_tts == TTS_PRESS) {
        gdc->set_color(gd, GFX_COLOR_PALETTE(3));
        gdc->filled_rect(gd, &rect, 1);
        gdc->set_color(gd, GFX_COLOR_PALETTE(1));
      }
    }
    gdc->rect(gd, &rect, 1);

    pos.x += (ts->ts_view.siz.width - ts->ts_indent * 40) / 2 - size.width / 2;
    pos.y += 10;
    gdc->text(gd, &pos, ts->ts_font, str, len);
  }

  ts->ts_pos.y += size.height + 20;
  return hit;
}


uint32_t
tig_hsv_to_rgb(int H, uint8_t S, uint8_t V)
{
  if(S == 0) {
    return (V << 16) | (V << 8) | V;
  }

  const int region = H / 60;
  const int beta = (H - region * 60) * 1110;

  const uint8_t p = (V * (255 - S)) >> 8;
  const uint8_t q = (V * (255 - ((S * beta) >> 16))) >> 8;
  const uint8_t t = (V * (255 - ((S * (65535 - beta)) >> 16))) >> 8;

  uint8_t r, g, b;

  switch(region) {
  case 0:
    r = V; g = t; b = p;
    break;
  case 1:
    r = q; g = V; b = p;
    break;
  case 2:
    r = p; g = V; b = t;
    break;
  case 3:
    r = p; g = q; b = V;
    break;
  case 4:
    r = t; g = p; b = V;
    break;
  default:
    r = V; g = p; b = q;
    break;
  }
  return (r << 16) | (g << 8) | b;
}
