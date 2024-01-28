#pragma once

#include <mios/error.h>
#include <stdint.h>
#include <stddef.h>

#include "http_util.h"

struct stream;

#define HTTP_FAIL_ON_ERROR 0x1

error_t http_get(const char *url, struct stream *output,
                 uint16_t flags, void *opaque,
                 const http_header_callback_t *header_callbacks,
                 size_t num_header_callbacks);
