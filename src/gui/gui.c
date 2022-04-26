#include "gui.h"

#include <assert.h>
#include <stdlib.h>
#include <sys/param.h>
#include <string.h>

#include <stdio.h>

static int
gui_pos_inside_rect(const gui_position_t *p, const gui_rect_t *r)
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
gui_display_init(gui_display_t *gd)
{
  pthread_mutex_init(&gd->gd_mutex, NULL);
  gd->gd_palette[1] = 0xffffff;
}


void *
gui_create_from_classdef(gui_widget_t *p,
                         const gui_widget_class_t *gwc)
{
  gui_widget_t *w = calloc(1, gwc->instance_size);
  w->gw_class = gwc;
  w->gw_weight = 1;
  if(p) {
    assert(p->gw_class->add_child != NULL);
    p->gw_class->add_child(p, w);
  }
  return w;
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
gui_set_rect(gui_widget_t *gw, const gui_rect_t *r)
{
  int x = r->pos.x + gw->gw_margin_left;
  int y = r->pos.y + gw->gw_margin_top;

  int w = r->siz.width  - gw->gw_margin_left - gw->gw_margin_right;
  int h = r->siz.height - gw->gw_margin_top - gw->gw_margin_bottom;

  if(gw->gw_alignment) {

    // Horizontal
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
gui_draw(gui_widget_t *w, gui_display_t *gd)
{
  if(w->gw_flags & GUI_WIDGET_NEED_LAYOUT) {
    w->gw_flags &= ~GUI_WIDGET_NEED_LAYOUT;
    if(w->gw_class->layout)
      w->gw_class->layout(w, gd);
  }
  w->gw_class->draw(w, gd);
}


void
gui_update_req(gui_widget_t *w, gui_display_t *gd)
{
  if(w->gw_flags & GUI_WIDGET_NEED_UPDATE_REQ) {
    w->gw_flags &= ~GUI_WIDGET_NEED_UPDATE_REQ;
    if(w->gw_class->update_req)
      w->gw_class->update_req(w, gd);
  }
}

void
gui_draw_display(gui_display_t *gd, const gui_rect_t *r)
{
  gui_update_req(gd->gd_root, gd);
  gui_set_rect(gd->gd_root, r);
  gui_draw(gd->gd_root, gd);
}


/*******************************************************
 * When touch starts the currently hit widget gets "grabbed"
 *
 * All remaining events are sent to it until release
 *
 * Buttons typically doesn't react if release is outside hitbox
 *
 */

void
gui_touch_release(gui_display_t *gd)
{
  if(gd->gd_grab == NULL)
    return;
  gd->gd_grab->gw_class->release(gd->gd_grab, gd);
  gd->gd_grab = NULL;
}


void
gui_touch_press(gui_display_t *gd, const gui_position_t *p)
{
  if(gd->gd_grab == NULL) {
    gd->gd_grab = gd->gd_root->gw_class->grab(gd->gd_root, gd, p, 1);
    printf("Grab widget %p\n", gd->gd_grab);
  } else {
    gd->gd_grab->gw_class->move(gd->gd_grab, gd, p);
  }
}



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
gui_container_draw(gui_widget_t *w, gui_display_t *gd)
{
  gui_container_t *gc = (gui_container_t *)w;
  gui_widget_t *c;
  STAILQ_FOREACH(c, &gc->gc_children, gw_parent_link) {
    gui_draw(c, gd);
  }
}


static void
gui_container_update_req(gui_container_t *gc, gui_display_t *gd, int mask)
{
  gui_widget_t *c;

  gc->gw.gw_req_size.width = 0;
  gc->gw.gw_req_size.height = 0;
  gc->gw.gw_flags &= ~(GUI_WIDGET_CONSTRAIN_X | GUI_WIDGET_CONSTRAIN_Y);

  STAILQ_FOREACH(c, &gc->gc_children, gw_parent_link) {
    gui_update_req(c, gd);

    if((c->gw_flags & mask) & GUI_WIDGET_CONSTRAIN_X) {
      const int w =
        c->gw_margin_left + c->gw_margin_right + c->gw_req_size.width;
      gc->gw.gw_req_size.width = MAX(gc->gw.gw_req_size.width, w);
      gc->gw.gw_flags |= GUI_WIDGET_CONSTRAIN_X;
    }

    if((c->gw_flags & mask) & GUI_WIDGET_CONSTRAIN_Y) {
      const int h =
        c->gw_margin_top + c->gw_margin_bottom + c->gw_req_size.height;
      gc->gw.gw_req_size.height = MAX(gc->gw.gw_req_size.height, h);
      gc->gw.gw_flags |= GUI_WIDGET_CONSTRAIN_Y;
    }
  }
}


static void
gui_container_layout(gui_widget_t *w, gui_display_t *gd)
{
  gui_container_t *gc = (gui_container_t *)w;
  gui_widget_t *c;
  STAILQ_FOREACH(c, &gc->gc_children, gw_parent_link) {
    gui_set_rect(c, &w->gw_rect);
  }
}


static gui_widget_t *
gui_container_grab(gui_widget_t *w, gui_display_t *gd, const gui_position_t *p,
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
gui_hbox_layout(gui_widget_t *w, gui_display_t *gd)
{
  gui_container_t *gc = (gui_container_t *)w;
  gui_widget_t *c;
  int wsum = 0;
  int rsum = 0;

  STAILQ_FOREACH(c, &gc->gc_children, gw_parent_link) {
    if(c->gw_flags & GUI_WIDGET_CONSTRAIN_X) {
      rsum += c->gw_req_size.width + c->gw_margin_left + c->gw_margin_right;
    } else {
      wsum += c->gw_weight;
    }
  }

  if(wsum == 0)
    wsum = 1;

  int wsiz = w->gw_rect.siz.width - rsum;
  gui_rect_t r = w->gw_rect;

  STAILQ_FOREACH(c, &gc->gc_children, gw_parent_link) {
    if(c->gw_flags & GUI_WIDGET_CONSTRAIN_X) {
      r.siz.width = c->gw_req_size.width + c->gw_margin_left + c->gw_margin_right;
    } else {
      r.siz.width = wsiz * c->gw_weight / wsum;
    }
    gui_set_rect(c, &r);
    r.pos.x += r.siz.width;
  }
}

static void
gui_vbox_layout(gui_widget_t *w, gui_display_t *gd)
{
  gui_container_t *gc = (gui_container_t *)w;
  gui_widget_t *c;
  int wsum = 0;
  int rsum = 0;

  STAILQ_FOREACH(c, &gc->gc_children, gw_parent_link) {
    if(c->gw_flags & GUI_WIDGET_CONSTRAIN_Y) {
      rsum += c->gw_req_size.height + c->gw_margin_top + c->gw_margin_bottom;
    } else {
      wsum += c->gw_weight;
    }
  }

  if(wsum == 0)
    wsum = 1;

  int hsiz = w->gw_rect.siz.height - rsum;
  gui_rect_t r = w->gw_rect;

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
gui_hbox_update_req(gui_widget_t *w, gui_display_t *gd)
{
  gui_container_t *gc = (gui_container_t *)w;
  gui_container_update_req(gc, gd, GUI_WIDGET_CONSTRAIN_Y);
}

static void
gui_vbox_update_req(gui_widget_t *w, gui_display_t *gd)
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
  gui_scalar_t ygrab;
  gui_scalar_t ypos;

  int yspeed;

} gui_list_t;


static const gui_widget_class_t gui_list_class;

static void
gui_list_layout(gui_widget_t *w, gui_display_t *gd)
{
  gui_list_t *gl = (gui_list_t *)w;
  gui_widget_t *c;

  const int def_height = w->gw_rect.siz.height / 4;
  const int y1 = w->gw_rect.pos.y;
  const int y2 = y1 + w->gw_rect.siz.height;
  int ypos = w->gw_rect.pos.y + gl->ypos;

  gui_rect_t r = w->gw_rect;
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
gui_list_draw(gui_widget_t *w, gui_display_t *gd)
{
  const gui_display_class_t *gdc = gd->gd_class;
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
gui_list_grab(gui_widget_t *w, gui_display_t *gd,
              const gui_position_t *p,
              int descend)
{
  if(!gui_pos_inside_rect(p, &w->gw_rect))
    return NULL;

  gui_list_t *gl = (gui_list_t *)w;
  gl->ygrab = p->y;
  return w;
}

static void
gui_list_move(gui_widget_t *w, gui_display_t *gd, const gui_position_t *p)
{
  gui_list_t *gl = (gui_list_t *)w;

  int scroll = p->y - gl->ygrab;
  gl->ygrab = p->y;
  gl->ypos += scroll;
  printf("ypos:%d scroll:%d\n", gl->ypos, scroll);
  w->gw_flags |= GUI_WIDGET_NEED_LAYOUT;
}

static void
gui_list_release(gui_widget_t *w, gui_display_t *gd)
{

}



static void
gui_list_update_req(gui_widget_t *w, gui_display_t *gd)
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
gui_abox_draw(gui_widget_t *w, gui_display_t *gd)
{
  const gui_display_class_t *gdc = gd->gd_class;
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
gui_abox_grab(gui_widget_t *w, gui_display_t *gd, const gui_position_t *p,
              int descend)
{
  if(!descend)
    return NULL;

  gui_container_t *gc = (gui_container_t *)w;
  gui_widget_t *c = STAILQ_LAST(&gc->gc_children, gui_widget, gw_parent_link);
  return c ? c->gw_class->grab(c, gd, p, descend) : NULL;
}

static void
gui_zbox_update_req(gui_widget_t *w, gui_display_t *gd)
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



/*****************************************************************
 * Quad
 */

typedef struct {
  gui_widget_t w;
  uint32_t bg_color;
  uint32_t border_color;
  uint32_t border_linesize;
} gui_quad_t;

static const gui_widget_class_t gui_quad_class;

static void
gui_quad_draw(gui_widget_t *w, gui_display_t *gd)
{
  gui_quad_t *gq = (gui_quad_t *)w;
  const gui_display_class_t *gdc = gd->gd_class;

  gdc->push_state(gd);
  gdc->set_color(gd, gq->bg_color);
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
  gq->bg_color = background_color;
  gq->border_color = border_color;
  gq->border_linesize = border_linesize;
  return &gq->w;
}





gui_widget_t *gui_create_quad_border(gui_widget_t *p,
                                     uint32_t bgcolor,
                                     uint32_t fgcolor);


/*****************************************************************
 * constant string
 */

typedef struct {
  gui_widget_t w;
  const char *str;
  size_t len;
  gui_font_id_t font_id;
} gui_cstr_t;

static const gui_widget_class_t gui_cstr_class;

static void
gui_cstr_draw(gui_widget_t *w, gui_display_t *gd)
{
  gui_cstr_t *gq = (gui_cstr_t *)w;
  const gui_display_class_t *gdc = gd->gd_class;

  gdc->text(gd, &w->gw_rect.pos, gq->font_id, gq->str, gq->len);
}

static void
gui_cstr_update_req(gui_widget_t *w, gui_display_t *gd)
{
  gui_cstr_t *gq = (gui_cstr_t *)w;
  const gui_display_class_t *gdc = gd->gd_class;
  w->gw_req_size = gdc->get_text_size(gd, gq->font_id, gq->str, gq->len);
  w->gw_flags |= GUI_WIDGET_CONSTRAIN_Y;
}


static const gui_widget_class_t gui_cstr_class = {
  .instance_size = sizeof(gui_cstr_t),
  .update_req = gui_cstr_update_req,
  .draw = gui_cstr_draw,
};

gui_widget_t *
gui_create_cstr(gui_widget_t *p, const char *str, gui_font_id_t font_id)
{
  gui_cstr_t *gq = gui_create_from_classdef(p, &gui_cstr_class);
  gq->font_id = font_id;
  gq->str = str;
  gq->len = strlen(str);
  gui_attrib_changed(&gq->w);
  return &gq->w;
}



/*****************************************************************
 * constant string
 */

static const gui_widget_class_t gui_vsep_class;

static void
gui_vsep_draw(gui_widget_t *w, gui_display_t *gd)
{
  const gui_display_class_t *gdc = gd->gd_class;

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
