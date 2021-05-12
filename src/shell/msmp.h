#pragma once

struct stream;

#define MSMP_CONSOLE 0x1

void msmp_init(struct stream *mux, int flags);

void msmp_send(uint8_t hdr, const void *data, size_t len);
