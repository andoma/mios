#pragma once

#include <mios/gfx.h>

// TIG - Tiny Immediate-mode Gui

typedef struct tig_ctx tig_ctx_t;

void *tig_make_delegate(void (*draw)(tig_ctx_t *ctx));

extern const gfx_display_delegate_t tig_display_delegate;

// Draw methods

void tig_text(tig_ctx_t *ctx, const char *fmt, ...) __attribute__ ((format(printf, 2, 3)));

void tig_move_abs(tig_ctx_t *tc, int x, int y);

void tig_move_rel(tig_ctx_t *tc, int x, int y);
