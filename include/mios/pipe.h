#pragma once

#include <mios/error.h>

struct stream;

#define PIPE_BACKGROUND 0x1

#define PIPE_NO_ESCAPE_CHARACTER -1

error_t pipe_bidir(struct stream *a, struct stream *b, const char *name,
                   int flags, int escape_character);
