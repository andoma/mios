#pragma once

#include <mios/error.h>

struct stream;

#define SPLICE_BACKGROUND 0x1

#define SPLICE_NO_ESCAPE_CHARACTER -1

error_t splice_bidir(struct stream *a, struct stream *b, const char *name,
                     int flags, int escape_character);
