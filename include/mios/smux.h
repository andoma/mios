#pragma once

#include <stdint.h>
#include <stddef.h>

struct stream;

void smux_create(struct stream *muxed, uint8_t delimiter, uint8_t reset_token,
                 size_t count, const uint8_t *idvec,
                 struct stream **streamvec);
