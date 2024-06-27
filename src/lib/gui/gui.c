#include "gui.h"

#include <assert.h>
#include <stdlib.h>
#include <sys/param.h>
#include <string.h>

#include <stdio.h>

static int
gui_pos_inside_rect(const gfx_position_t *p, const gfx_rect_t *r)
{
  return p->x >= r->pos.x && p->x < r->pos.x + r->siz.width &&
    p->y >= r->pos.y && p->y < r->pos.y + r->siz.height;
}



void
gui_set_alignment(gui_widget_t *gw, uint8_t alignment)
{
  gw->gw_alignment = alignment;
  gui_attrib_changed(gw);
}


void
gui_set_margin(gui_widget_t *gw,
               int8_t top, int8_t right,
               int8_t bottom, int8_t left)
{
  gw->gw_margin_top    = top;
  gw->gw_margin_right  = right;
  gw->gw_margin_bottom = bottom;
  gw->gw_margin_left   = left;
}


void
gui_set_margin_all(gui_widget_t *gw, int8_t margin)
{
  gui_set_margin(gw, margin, margin, margin, margin);
}


void
gui_display_init(gfx_display_t *gd)
{
  pthread_mutex_init(&gd->gd_mutex, NULL);
  gd->gd_palette[1] = 0xffffff;
  gd->gd_palette[2] = 0x444444;
}


static void *
gui_create_from_classdefv(gui_widget_t *p,
                          const gui_widget_class_t *gwc,
                          size_t extra)
{
  gui_widget_t *w = calloc(1, gwc->instance_size + extra);
  w->gw_class = gwc;
  w->gw_weight = 1;
  if(p) {
    assert(p->gw_class->add_child != NULL);
    p->gw_class->add_child(p, w);
  }
  return w;
}


void *
gui_create_from_classdef(gui_widget_t *p,
                         const gui_widget_class_t *gwc)
{
  return gui_create_from_classdefv(p, gwc, 0);
}


void
gui_attrib_changed(gui_widget_t *w)
{
  while(w) {
    w->gw_flags |= GUI_WIDGET_NEED_UPDATE_REQ;
    w = w->gw_parent;
  }
}

void
gui_set_rect(gui_widget_t *gw, const gfx_rect_t *r)
{
  int x = r->pos.x + gw->gw_margin_left;
  int y = r->pos.y + gw->gw_margin_top;

  int w = r->siz.width  - gw->gw_margin_left - gw->gw_margin_right;
  int h = r->siz.height - gw->gw_margin_top - gw->gw_margin_bottom;

  if(gw->gw_alignment) {

    // Horizontal
    if(gw->gw_req_size.width) {
      switch(gw->gw_alignment) {
      case GW_ALIGN_BOTTOM_LEFT:
      case GW_ALIGN_MID_LEFT:
      case GW_ALIGN_TOP_LEFT:
        break;

      case GW_ALIGN_BOTTOM_CENTER:
      case GW_ALIGN_MID_CENTER:
      case GW_ALIGN_TOP_CENTER:
        x += (w - gw->gw_req_size.width) / 2;
        break;
      case GW_ALIGN_BOTTOM_RIGHT:
      case GW_ALIGN_MID_RIGHT:
      case GW_ALIGN_TOP_RIGHT:
        x += w - gw->gw_req_size.width;
        break;
      }
      w = gw->gw_req_size.width;
    }

    if(gw->gw_req_size.height) {
      switch(gw->gw_alignment) {
      case GW_ALIGN_BOTTOM_LEFT:
      case GW_ALIGN_BOTTOM_CENTER:
      case GW_ALIGN_BOTTOM_RIGHT:
        y += h - gw->gw_req_size.height;
        break;
      case GW_ALIGN_MID_LEFT:
      case GW_ALIGN_MID_CENTER:
      case GW_ALIGN_MID_RIGHT:
        y += (h - gw->gw_req_size.height) / 2;
        break;
      case GW_ALIGN_TOP_LEFT:
      case GW_ALIGN_TOP_CENTER:
      case GW_ALIGN_TOP_RIGHT:
        break;
      }
      h = gw->gw_req_size.height;
    }
  }

  if(x == gw->gw_rect.pos.x &&
     y == gw->gw_rect.pos.y &&
     w == gw->gw_rect.siz.width &&
     h == gw->gw_rect.siz.height)
    return;
  gw->gw_rect.pos.x = x;
  gw->gw_rect.pos.y = y;
  gw->gw_rect.siz.width = w;
  gw->gw_rect.siz.height = h;
  gw->gw_flags |= GUI_WIDGET_NEED_LAYOUT;
}


void
gui_draw(gui_widget_t *w, gfx_display_t *gd)
{
  if(w->gw_flags & GUI_WIDGET_NEED_LAYOUT) {
    w->gw_flags &= ~GUI_WIDGET_NEED_LAYOUT;
    if(w->gw_class->layout)
      w->gw_class->layout(w, gd);
  }
  w->gw_class->draw(w, gd);
}


void
gui_update_req(gui_widget_t *w, gfx_display_t *gd)
{
  if(w->gw_flags & GUI_WIDGET_NEED_UPDATE_REQ) {
    w->gw_flags &= ~GUI_WIDGET_NEED_UPDATE_REQ;
    if(w->gw_class->update_req)
      w->gw_class->update_req(w, gd);
  }
}


/***********************************************************************
 *
 * GFX delegates
 *
 */


static void
gui_draw_display(void *opaque, gfx_display_t *gd, const gfx_rect_t *r)
{
  gui_root_t *gr = opaque;

  gui_update_req(gr->gr_root, gd);
  gui_set_rect(gr->gr_root, r);
  gui_draw(gr->gr_root, gd);
}


/*
 * When touch starts the currently hit widget gets "grabbed".
 * All remaining events are sent to it until release.
 * Buttons typically doesn't react if release is outside hitbox.
 */

static void
gui_touch_release(void *opaque, gfx_display_t *gd)
{
  gui_root_t *gr = opaque;

  if(gr->gr_grab == NULL)
    return;
  gr->gr_grab->gw_class->release(gr->gr_grab, gd);
  gr->gr_grab = NULL;
}


static void
gui_touch_press(void *opaque, gfx_display_t *gd, const gfx_position_t *p)
{
  gui_root_t *gr = opaque;

  if(gr->gr_grab == NULL) {
    gr->gr_grab = gr->gr_root->gw_class->grab(gr->gr_root, gd, p, 1);
  } else {
    gr->gr_grab->gw_class->move(gr->gr_grab, gd, p);
  }
}


const gfx_display_delegate_t gui_display_delegate = {
  .gdd_draw = gui_draw_display,
  .gdd_pre = gui_prep_display,
  .gdd_touch_release = gui_touch_release,
  .gdd_touch_press = gui_touch_press,
};

/*****************************************************************
 * Container
 */

static void
gui_container_add_child(gui_widget_t *p, gui_widget_t *c)
{
  gui_container_t *gc = (gui_container_t *)p;
  c->gw_parent = p;
  STAILQ_INSERT_TAIL(&gc->gc_children, c, gw_parent_link);
}


static void
gui_container_draw(gui_widget_t *w, gfx_display_t *gd)
{
  gui_container_t *gc = (gui_container_t *)w;
  gui_widget_t *c;
  STAILQ_FOREACH(c, &gc->gc_children, gw_parent_link) {
    gui_draw(c, gd);
  }
}


static void
gui_container_update_req(gui_container_t *gc, gfx_display_t *gd, int mask)
{
  gui_widget_t *c;

  gc->gw.gw_req_size.width = 0;
  gc->gw.gw_req_size.height = 0;
  gc->gw.gw_flags &= ~(GUI_WIDGET_CONSTRAIN_X | GUI_WIDGET_CONSTRAIN_Y);

  int need_layout = 0;
  STAILQ_FOREACH(c, &gc->gc_children, gw_parent_link) {
    gui_update_req(c, gd);

    if((c->gw_flags & mask) & GUI_WIDGET_CONSTRAIN_X) {
      const int w =
        c->gw_margin_left + c->gw_margin_right + c->gw_req_size.width;
      gc->gw.gw_req_size.width = MAX(gc->gw.gw_req_size.width, w);
      gc->gw.gw_flags |= GUI_WIDGET_CONSTRAIN_X;
      need_layout = 1;
    }

    if((c->gw_flags & mask) & GUI_WIDGET_CONSTRAIN_Y) {
      const int h =
        c->gw_margin_top + c->gw_margin_bottom + c->gw_req_size.height;
      gc->gw.gw_req_size.height = MAX(gc->gw.gw_req_size.height, h);
      gc->gw.gw_flags |= GUI_WIDGET_CONSTRAIN_Y;
      need_layout = 1;
    }
  }
  if(need_layout)
    gc->gw.gw_flags |= GUI_WIDGET_NEED_LAYOUT;
}


static void
gui_container_layout(gui_widget_t *w, gfx_display_t *gd)
{
  gui_container_t *gc = (gui_container_t *)w;
  gui_widget_t *c;
  STAILQ_FOREACH(c, &gc->gc_children, gw_parent_link) {
    gui_set_rect(c, &w->gw_rect);
  }
}


static gui_widget_t *
gui_container_grab(gui_widget_t *w, gfx_display_t *gd, const gfx_position_t *p,
                   int descend)
{
  gui_container_t *gc = (gui_container_t *)w;
  gui_widget_t *c;

  if(!descend)
    return NULL;
  STAILQ_FOREACH(c, &gc->gc_children, gw_parent_link) {
    if(c->gw_class->grab == NULL)
      continue;
    gui_widget_t *g = c->gw_class->grab(c, gd, p, descend);
    if(g != NULL)
      return g;
  }
  return NULL;
}



/*****************************************************************
 * Box
 */

static const gui_widget_class_t gui_hbox_class;
static const gui_widget_class_t gui_vbox_class;

static void
gui_hbox_layout(gui_widget_t *w, gfx_display_t *gd)
{
  gui_container_t *gc = (gui_container_t *)w;
  gui_widget_t *c;
  int wsum = 0;
  int rsum = 0;
  const gui_widget_t *b = NULL;

  STAILQ_FOREACH(c, &gc->gc_children, gw_parent_link) {
    if(c->gw_flags & GUI_WIDGET_CONSTRAIN_X) {
      rsum += c->gw_req_size.width + c->gw_margin_left + c->gw_margin_right;
    } else {
      wsum += c->gw_weight;
    }
    if(c->gw_flags & GUI_WIDGET_BASELINE) {
      if(b == NULL || c->gw_baseline > b->gw_baseline)
        b = c;
    }
  }
  if(wsum == 0)
    wsum = 1;

  int wsiz = w->gw_rect.siz.width - rsum;
  gfx_rect_t r = w->gw_rect;

  STAILQ_FOREACH(c, &gc->gc_children, gw_parent_link) {
    if(c->gw_flags & GUI_WIDGET_CONSTRAIN_X) {
      r.siz.width = c->gw_req_size.width + c->gw_margin_left + c->gw_margin_right;
    } else {
      r.siz.width = wsiz * c->gw_weight / wsum;
    }
    gui_set_rect(c, &r);
    r.pos.x += r.siz.width;
  }

  if(b != NULL) {
    STAILQ_FOREACH(c, &gc->gc_children, gw_parent_link) {
      if(c == b || !(c->gw_flags & GUI_WIDGET_BASELINE))
        continue;

      c->gw_rect.pos.y =
        b->gw_rect.pos.y + (int)b->gw_baseline - (int)c->gw_baseline;
    }
  }
}

static void
gui_vbox_layout(gui_widget_t *w, gfx_display_t *gd)
{
  gui_container_t *gc = (gui_container_t *)w;
  gui_widget_t *c;
  int wsum = 0;
  int rsum = 0;

  const int debug = !!(w->gw_flags & GUI_WIDGET_DEBUG);

  STAILQ_FOREACH(c, &gc->gc_children, gw_parent_link) {
    if(c->gw_flags & GUI_WIDGET_CONSTRAIN_Y) {
      rsum += c->gw_req_size.height + c->gw_margin_top + c->gw_margin_bottom;

      if(debug)
        printf("vbox@%p: child@%p constrainted y: %d+%d+%d\n",
               w, c, c->gw_req_size.height, c->gw_margin_top, c->gw_margin_bottom);

    } else {
      wsum += c->gw_weight;

      if(debug)
        printf("vbox@%p: child@%p weight: %d\n",
               w, c, c->gw_weight);
    }
  }

  if(wsum == 0)
    wsum = 1;

  int hsiz = w->gw_rect.siz.height - rsum;
  gfx_rect_t r = w->gw_rect;

  STAILQ_FOREACH(c, &gc->gc_children, gw_parent_link) {
    if(c->gw_flags & GUI_WIDGET_CONSTRAIN_Y) {
      r.siz.height = c->gw_req_size.height + c->gw_margin_top + c->gw_margin_bottom;
    } else {
      r.siz.height = hsiz * c->gw_weight / wsum;
    }
    gui_set_rect(c, &r);
    r.pos.y += r.siz.height;
  }
}


static void
gui_hbox_update_req(gui_widget_t *w, gfx_display_t *gd)
{
  gui_container_t *gc = (gui_container_t *)w;
  gui_container_update_req(gc, gd, GUI_WIDGET_CONSTRAIN_Y);
}

static void
gui_vbox_update_req(gui_widget_t *w, gfx_display_t *gd)
{
  gui_container_t *gc = (gui_container_t *)w;
  gui_container_update_req(gc, gd, GUI_WIDGET_CONSTRAIN_X);
}


static const gui_widget_class_t gui_hbox_class = {
  .instance_size = sizeof(gui_container_t),
  .update_req = gui_hbox_update_req,
  .layout = gui_hbox_layout,
  .draw = gui_container_draw,
  .add_child = gui_container_add_child,
  .grab = gui_container_grab,
};

static const gui_widget_class_t gui_vbox_class = {
  .instance_size = sizeof(gui_container_t),
  .update_req = gui_vbox_update_req,
  .layout = gui_vbox_layout,
  .draw = gui_container_draw,
  .add_child = gui_container_add_child,
  .grab = gui_container_grab,
};


gui_widget_t *
gui_create_hbox(gui_widget_t *p)
{
  gui_container_t *gc = gui_create_from_classdef(p, &gui_hbox_class);
  STAILQ_INIT(&gc->gc_children);
  return &gc->gw;
}

gui_widget_t *
gui_create_vbox(gui_widget_t *p)
{
  gui_container_t *gc = gui_create_from_classdef(p, &gui_vbox_class);
  STAILQ_INIT(&gc->gc_children);
  return &gc->gw;
}



/*****************************************************************
 * List
 */

typedef struct gui_list {
  gui_container_t gc;
  gfx_scalar_t ygrab;
  gfx_scalar_t ypos;

  int yspeed;

} gui_list_t;


static const gui_widget_class_t gui_list_class;

static void
gui_list_layout(gui_widget_t *w, gfx_display_t *gd)
{
  gui_list_t *gl = (gui_list_t *)w;
  gui_widget_t *c;

  const int def_height = w->gw_rect.siz.height / 4;
  const int y1 = w->gw_rect.pos.y;
  const int y2 = y1 + w->gw_rect.siz.height;
  int ypos = w->gw_rect.pos.y + gl->ypos;

  gfx_rect_t r = w->gw_rect;
  STAILQ_FOREACH(c, &gl->gc.gc_children, gw_parent_link) {
    int h;
    if(c->gw_flags & GUI_WIDGET_CONSTRAIN_Y) {
      h = c->gw_req_size.height + c->gw_margin_top + c->gw_margin_bottom;
    } else {
      h = def_height;
    }

    if(ypos + h > y1 && ypos < y2) {
      c->gw_flags &= ~GUI_WIDGET_HIDDEN_BY_PARENT;
      r.pos.y = ypos;
      r.siz.height = h;
      gui_set_rect(c, &r);
    } else {
      c->gw_flags |= GUI_WIDGET_HIDDEN_BY_PARENT;
    }

    ypos += h;
  }
}

void
gui_list_draw(gui_widget_t *w, gfx_display_t *gd)
{
  const gfx_display_class_t *gdc = gd->gd_class;
  gui_container_t *gc = (gui_container_t *)w;
  gui_widget_t *c;

  gdc->push_state(gd);
  gdc->scissor(gd, &w->gw_rect);
  STAILQ_FOREACH(c, &gc->gc_children, gw_parent_link) {
    if(c->gw_flags & GUI_WIDGET_HIDDEN_BY_PARENT)
      continue;
    gui_draw(c, gd);
  }
  gdc->pop_state(gd);
}


static gui_widget_t *
gui_list_grab(gui_widget_t *w, gfx_display_t *gd,
              const gfx_position_t *p,
              int descend)
{
  if(!gui_pos_inside_rect(p, &w->gw_rect))
    return NULL;

  gui_list_t *gl = (gui_list_t *)w;
  gl->ygrab = p->y;
  return w;
}

static void
gui_list_move(gui_widget_t *w, gfx_display_t *gd, const gfx_position_t *p)
{
  gui_list_t *gl = (gui_list_t *)w;

  int scroll = p->y - gl->ygrab;
  gl->ygrab = p->y;
  gl->ypos += scroll;
  printf("ypos:%d scroll:%d\n", gl->ypos, scroll);
  w->gw_flags |= GUI_WIDGET_NEED_LAYOUT;
}

static void
gui_list_release(gui_widget_t *w, gfx_display_t *gd)
{

}



static void
gui_list_update_req(gui_widget_t *w, gfx_display_t *gd)
{
  gui_container_t *gc = (gui_container_t *)w;
  gui_container_update_req(gc, gd, 0);
}


static const gui_widget_class_t gui_list_class = {
  .instance_size = sizeof(gui_list_t),
  .update_req = gui_list_update_req,
  .layout = gui_list_layout,
  .draw = gui_list_draw,
  .add_child = gui_container_add_child,
  .grab = gui_list_grab,
  .move = gui_list_move,
  .release = gui_list_release,
};


gui_widget_t *
gui_create_list(gui_widget_t *p)
{
  gui_list_t *gl = gui_create_from_classdef(p, &gui_list_class);
  STAILQ_INIT(&gl->gc.gc_children);
  return &gl->gc.gw;
}



/*****************************************************************
 * zBox
 */

static const gui_widget_class_t gui_zbox_class;
static const gui_widget_class_t gui_abox_class;


static void
gui_abox_draw(gui_widget_t *w, gfx_display_t *gd)
{
  const gfx_display_class_t *gdc = gd->gd_class;
  gui_container_t *gc = (gui_container_t *)w;

  gui_widget_t *a = STAILQ_FIRST(&gc->gc_children);
  if(a != NULL) {
    gui_widget_t *b = STAILQ_NEXT(a, gw_parent_link);

    if(b) {
      gdc->push_state(gd);
      gdc->set_alpha(gd, 64);
    }
    gui_draw(a, gd);
    if(b) {
      gdc->pop_state(gd);
      gui_draw(b, gd);
    }
  }
}

static gui_widget_t *
gui_abox_grab(gui_widget_t *w, gfx_display_t *gd, const gfx_position_t *p,
              int descend)
{
  if(!descend)
    return NULL;

  gui_container_t *gc = (gui_container_t *)w;
  gui_widget_t *c = STAILQ_LAST(&gc->gc_children, gui_widget, gw_parent_link);
  return c ? c->gw_class->grab(c, gd, p, descend) : NULL;
}

static void
gui_zbox_update_req(gui_widget_t *w, gfx_display_t *gd)
{
  gui_container_t *gc = (gui_container_t *)w;
  gui_container_update_req(gc, gd,
                           GUI_WIDGET_CONSTRAIN_X | GUI_WIDGET_CONSTRAIN_Y);
}


static const gui_widget_class_t gui_zbox_class = {
  .instance_size = sizeof(gui_container_t),
  .update_req = gui_zbox_update_req,
  .layout = gui_container_layout,
  .draw = gui_container_draw,
  .add_child = gui_container_add_child,
  .grab = gui_container_grab,
};

static const gui_widget_class_t gui_abox_class = {
  .instance_size = sizeof(gui_container_t),
  .update_req = gui_zbox_update_req,
  .layout = gui_container_layout,
  .draw = gui_abox_draw,
  .add_child = gui_container_add_child,
  .grab = gui_abox_grab,
};


gui_widget_t *
gui_create_zbox(gui_widget_t *p)
{
  gui_container_t *gc = gui_create_from_classdef(p, &gui_zbox_class);
  STAILQ_INIT(&gc->gc_children);
  return &gc->gw;
}


gui_widget_t *
gui_create_abox(gui_widget_t *p)
{
  gui_container_t *gc = gui_create_from_classdef(p, &gui_abox_class);
  STAILQ_INIT(&gc->gc_children);
  return &gc->gw;
}


typedef struct {
  gui_widget_t w;
  uint32_t color;
} gui_colored_widget_t;


void
gui_set_color(gui_widget_t *w, uint32_t color)
{
  gui_colored_widget_t *gcw = (gui_colored_widget_t *)w;
  gcw->color = color;
}

/*****************************************************************
 * Quad
 */

typedef struct {
  gui_colored_widget_t cw;
  uint32_t border_color;
  uint32_t border_linesize;
} gui_quad_t;

static const gui_widget_class_t gui_quad_class;

static void
gui_quad_draw(gui_widget_t *w, gfx_display_t *gd)
{
  gui_quad_t *gq = (gui_quad_t *)w;
  const gfx_display_class_t *gdc = gd->gd_class;

  gdc->push_state(gd);
  gdc->set_color(gd, gq->cw.color);
  gdc->filled_rect(gd, &w->gw_rect, 1);
  if(gq->border_linesize) {
    gdc->set_color(gd, gq->border_color);
    gdc->rect(gd, &w->gw_rect, gq->border_linesize);
  }
  gdc->pop_state(gd);
}


static const gui_widget_class_t gui_quad_class = {
  .instance_size = sizeof(gui_quad_t),
  .draw = gui_quad_draw,
};

gui_widget_t *
gui_create_quad(gui_widget_t *p,
                uint32_t background_color,
                uint32_t border_color,
                uint32_t border_linesize)
{
  gui_quad_t *gq = gui_create_from_classdef(p, &gui_quad_class);
  gq->cw.color = background_color;
  gq->border_color = border_color;
  gq->border_linesize = border_linesize;
  return &gq->cw.w;
}





gui_widget_t *gui_create_quad_border(gui_widget_t *p,
                                     uint32_t bgcolor,
                                     uint32_t fgcolor);


/*****************************************************************
 * constant string
 */

typedef struct {
  gui_colored_widget_t cw;
  size_t len;
  gfx_font_id_t font_id;
  union {
    const char *cstr;
    struct {
      size_t maxlen;
      char dstr[0];
    };
  };
} gui_str_t;


static const gui_widget_class_t gui_cstr_class;
static const gui_widget_class_t gui_dstr_class;


static void
gui_str_draw(gui_str_t *gs, gfx_display_t *gd, const char *str)
{
  const gfx_display_class_t *gdc = gd->gd_class;

  if(gs->cw.color != 0xffffffff) {
    gdc->push_state(gd);
    gdc->set_color(gd, gs->cw.color);
  }
  gdc->text(gd, &gs->cw.w.gw_rect.pos, gs->font_id, str, gs->len);
  if(gs->cw.color != 0xffffffff) {
    gdc->pop_state(gd);
  }
}


static void
gui_cstr_draw(gui_widget_t *w, gfx_display_t *gd)
{
  gui_str_t *gs = (gui_str_t *)w;
  gui_str_draw(gs, gd, gs->cstr);
}


static void
gui_dstr_draw(gui_widget_t *w, gfx_display_t *gd)
{
  gui_str_t *gs = (gui_str_t *)w;
  gui_str_draw(gs, gd, gs->dstr);
}


static void
gui_str_update_req(gui_str_t *gs, gfx_display_t *gd, const char *str)
{
  const gfx_display_class_t *gdc = gd->gd_class;
  gs->cw.w.gw_req_size = gdc->get_text_size(gd, gs->font_id, str, gs->len);
  gs->cw.w.gw_baseline = gdc->get_font_baseline(gd, gs->font_id);
  gs->cw.w.gw_flags |= GUI_WIDGET_CONSTRAIN_Y | GUI_WIDGET_BASELINE;
}


static void
gui_cstr_update_req(gui_widget_t *w, gfx_display_t *gd)
{
  gui_str_t *gs = (gui_str_t *)w;
  gui_str_update_req(gs, gd, gs->cstr);
}


static void
gui_dstr_update_req(gui_widget_t *w, gfx_display_t *gd)
{
  gui_str_t *gs = (gui_str_t *)w;
  gui_str_update_req(gs, gd, gs->dstr);

}

static const gui_widget_class_t gui_cstr_class = {
  .instance_size = sizeof(gui_str_t),
  .update_req = gui_cstr_update_req,
  .draw = gui_cstr_draw,
};

static const gui_widget_class_t gui_dstr_class = {
  .instance_size = sizeof(gui_str_t),
  .update_req = gui_dstr_update_req,
  .draw = gui_dstr_draw,
};


gui_widget_t *
gui_create_cstr(gui_widget_t *p, const char *str, gfx_font_id_t font_id,
                uint8_t alignment)
{
  gui_str_t *gc = gui_create_from_classdef(p, &gui_cstr_class);
  gc->font_id = font_id;
  gc->cstr = str;
  gc->len = strlen(str);
  gc->cw.w.gw_alignment = alignment;
  gc->cw.color = 0xffffffff;
  gui_attrib_changed(&gc->cw.w);
  return &gc->cw.w;
}


gui_widget_t *
gui_create_dstr(gui_widget_t *p, size_t maxlen, gfx_font_id_t font_id,
                uint8_t alignment)
{
  gui_str_t *gd = gui_create_from_classdefv(p, &gui_dstr_class, maxlen + 1);
  gd->font_id = font_id;
  gd->maxlen = maxlen;
  gd->len = 0;
  gd->cw.w.gw_alignment = alignment;
  gd->cw.color = 0xffffffff;
  return &gd->cw.w;
}


void
gui_set_dstr(gui_widget_t *w, const char *str)
{
  gui_str_t *gs = (gui_str_t *)w;
  gs->len = strlcpy(gs->dstr, str, gs->maxlen + 1);
  gui_attrib_changed(&gs->cw.w);
}



/*****************************************************************
 * vertical separator
 */

static const gui_widget_class_t gui_vsep_class;

static void
gui_vsep_draw(gui_widget_t *w, gfx_display_t *gd)
{
  const gfx_display_class_t *gdc = gd->gd_class;

  int y = w->gw_rect.pos.y + w->gw_rect.siz.height / 2;

  gdc->line(gd,
            w->gw_rect.pos.x, y,
            w->gw_rect.pos.x + w->gw_rect.siz.width, y,
            w->gw_req_size.height);
}

static const gui_widget_class_t gui_vsep_class = {
  .instance_size = sizeof(gui_widget_t),
  .draw = gui_vsep_draw,
};

gui_widget_t *
gui_create_vsep(gui_widget_t *p, int thickness, int margin)
{
  gui_widget_t *gw = gui_create_from_classdef(p, &gui_vsep_class);
  gw->gw_req_size.height = thickness;
  gw->gw_margin_top = margin;
  gw->gw_margin_bottom = margin;
  gw->gw_flags |= GUI_WIDGET_CONSTRAIN_Y;
  gui_attrib_changed(gw);
  return gw;
}

/*****************************************************************
 * horizontal separator
 */

static const gui_widget_class_t gui_hsep_class;

static void
gui_hsep_draw(gui_widget_t *w, gfx_display_t *gd)
{
  const gfx_display_class_t *gdc = gd->gd_class;

  int x = w->gw_rect.pos.x + w->gw_rect.siz.width / 2;

  gdc->line(gd,
            x, w->gw_rect.pos.y,
            x, w->gw_rect.pos.y + w->gw_rect.siz.height,
            w->gw_req_size.width);
}

static const gui_widget_class_t gui_hsep_class = {
  .instance_size = sizeof(gui_widget_t),
  .draw = gui_hsep_draw,
};

gui_widget_t *
gui_create_hsep(gui_widget_t *p, int thickness, int margin)
{
  gui_widget_t *gw = gui_create_from_classdef(p, &gui_hsep_class);
  gw->gw_req_size.width = thickness;
  gw->gw_margin_left = margin;
  gw->gw_margin_right = margin;
  gw->gw_flags |= GUI_WIDGET_CONSTRAIN_X;
  gui_attrib_changed(gw);
  return gw;
}

/*****************************************************************
 * bitmap
 */

typedef struct {
  gui_colored_widget_t cw;
  int bitmap_id;
} gui_bitmap_t;


static const gui_widget_class_t gui_bitmap_class;

static void
gui_bitmap_draw(gui_widget_t *w, gfx_display_t *gd)
{
  const gfx_display_class_t *gdc = gd->gd_class;
  gui_bitmap_t *gb = (gui_bitmap_t *)w;

  if(gb->cw.color != 0xffffffff) {
    gdc->push_state(gd);
    gdc->set_color(gd, gb->cw.color);
  }

  gdc->bitmap(gd, &w->gw_rect.pos, gb->bitmap_id);

  if(gb->cw.color != 0xffffffff) {
    gdc->pop_state(gd);
  }
}

static void
gui_bitmap_update_req(gui_widget_t *w, gfx_display_t *gd)
{
  const gfx_display_class_t *gdc = gd->gd_class;
  gui_bitmap_t *gb = (gui_bitmap_t *)w;

  w->gw_req_size = gdc->get_bitmap_size(gd, gb->bitmap_id);
  w->gw_flags |= GUI_WIDGET_CONSTRAIN_Y | GUI_WIDGET_CONSTRAIN_X;
}


static const gui_widget_class_t gui_bitmap_class = {
  .instance_size = sizeof(gui_bitmap_t),
  .update_req = gui_bitmap_update_req,
  .draw = gui_bitmap_draw,
};

gui_widget_t *
gui_create_bitmap(gui_widget_t *p, int bitmap)
{
  gui_bitmap_t *gb = gui_create_from_classdef(p, &gui_bitmap_class);
  gb->bitmap_id = bitmap;
  gb->cw.color = 0xffffffff;
  gui_attrib_changed(&gb->cw.w);
  return &gb->cw.w;
}
